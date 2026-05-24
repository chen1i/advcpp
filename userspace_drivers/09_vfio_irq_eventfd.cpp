// Exercise 09: VFIO IRQ eventfd
//
// Goal: connect a VFIO device interrupt to an eventfd and wait for it.
//
// Background
// ----------
// VFIO does not deliver device interrupts as Unix signals.  Instead, userspace
// gives VFIO an eventfd for a chosen IRQ index and vector:
//
//   eventfd() -> VFIO_DEVICE_SET_IRQS -> poll(eventfd)
//
// This sample can list the IRQ indexes reported by a vfio-pci device, bind one
// interrupt vector to an eventfd, optionally ask VFIO to trigger a kernel
// loopback event, and poll for completion.  It does not program device queues,
// write BAR registers, or start real device DMA.
//
// New concepts
// ------------
// - eventfd as a kernel-to-userspace notification primitive
// - VFIO_DEVICE_GET_IRQ_INFO
// - VFIO_DEVICE_SET_IRQS with VFIO_IRQ_SET_DATA_EVENTFD
// - poll() on an eventfd

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <linux/vfio.h>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }

  ~UniqueFd() { reset(); }

  int get() const { return fd_; }

  int release() {
    int old = fd_;
    fd_ = -1;
    return old;
  }

  void reset(int fd = -1) {
    if (fd_ != -1)
      ::close(fd_);
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

struct DriverInfo {
  bool bound = false;
  std::string name = "<unbound>";
  fs::path path;
};

struct IommuGroupInfo {
  std::string id;
  fs::path path;
};

struct VfioContext {
  UniqueFd container;
  UniqueFd group;
  UniqueFd device;
};

static std::string errno_text(int error) {
  return std::strerror(error);
}

static std::string normalize_bdf(std::string bdf) {
  if (std::count(bdf.begin(), bdf.end(), ':') == 1)
    bdf = "0000:" + bdf;
  return bdf;
}

static void usage(const char *argv0) {
  std::println(std::cerr,
               "Usage:\n"
               "  {} <BDF> --show\n"
               "  {} <BDF> --irq <index> [--vector <n>] [--wait-ms <ms>]\n"
               "     [--trigger-test] [--dry-run|--yes]\n\n"
               "Defaults:\n"
               "  vector = 0\n"
               "  wait-ms = 5000\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.6 --show\n"
               "  {} c1:00.6 --irq 2 --vector 0 --trigger-test --yes\n"
               "  {} c1:00.6 --irq 2 --vector 0 --wait-ms 10000 --yes",
               argv0, argv0, argv0, argv0, argv0);
}

static unsigned long long parse_ull(std::string_view text,
                                    std::string_view name) {
  std::size_t parsed = 0;
  std::string owned(text);
  unsigned long long value = std::stoull(owned, &parsed, 0);
  if (parsed != owned.size())
    throw std::runtime_error("invalid " + std::string(name) + ": " + owned);
  return value;
}

static __u32 parse_u32(std::string_view text, std::string_view name) {
  unsigned long long value = parse_ull(text, name);
  if (value > std::numeric_limits<__u32>::max()) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }
  return static_cast<__u32>(value);
}

static std::string join_flags(const std::vector<std::string_view> &flags) {
  if (flags.empty())
    return "none";

  std::string out;
  for (std::string_view flag : flags) {
    if (!out.empty())
      out += "|";
    out += flag;
  }
  return out;
}

static std::string irq_flag_names(__u32 flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_IRQ_INFO_EVENTFD)
    names.push_back("EVENTFD");
  if (flags & VFIO_IRQ_INFO_MASKABLE)
    names.push_back("MASKABLE");
  if (flags & VFIO_IRQ_INFO_AUTOMASKED)
    names.push_back("AUTOMASKED");
  if (flags & VFIO_IRQ_INFO_NORESIZE)
    names.push_back("NORESIZE");
  return join_flags(names);
}

static std::string irq_name(__u32 index) {
  switch (index) {
  case VFIO_PCI_INTX_IRQ_INDEX:
    return "INTx";
  case VFIO_PCI_MSI_IRQ_INDEX:
    return "MSI";
  case VFIO_PCI_MSIX_IRQ_INDEX:
    return "MSI-X";
  case VFIO_PCI_ERR_IRQ_INDEX:
    return "ERR";
  case VFIO_PCI_REQ_IRQ_INDEX:
    return "REQ";
  default:
    return "device-specific";
  }
}

static UniqueFd open_fd(const fs::path &path, int flags) {
  int fd = ::open(path.c_str(), flags | O_CLOEXEC);
  if (fd == -1) {
    int error = errno;
    std::string message = "cannot open " + path.string() + ": " +
                          errno_text(error);
    if (error == EBUSY && path.string().starts_with("/dev/vfio/")) {
      message +=
          "; the VFIO group is already open in another process. Check with "
          "fuser or lsof and stop the process that owns this group";
    }
    throw std::runtime_error(message);
  }
  return UniqueFd(fd);
}

static void ioctl_checked(int fd, unsigned long request, void *arg,
                          std::string_view operation) {
  if (::ioctl(fd, request, arg) == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             errno_text(error));
  }
}

static int ioctl_value(int fd, unsigned long request,
                       std::string_view operation) {
  int result = ::ioctl(fd, request);
  if (result == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             errno_text(error));
  }
  return result;
}

static int ioctl_arg_value(int fd, unsigned long request, unsigned long arg,
                           std::string_view operation) {
  int result = ::ioctl(fd, request, arg);
  if (result == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             errno_text(error));
  }
  return result;
}

static DriverInfo current_driver(const fs::path &dev_dir) {
  fs::path driver_link = dev_dir / "driver";
  if (!fs::exists(driver_link))
    return {};

  DriverInfo info;
  info.bound = true;

  fs::path target = fs::read_symlink(driver_link);
  info.name = target.filename().string();

  if (target.is_absolute())
    info.path = target;
  else
    info.path = fs::weakly_canonical(driver_link.parent_path() / target);

  return info;
}

static void require_vfio_driver(const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);
  if (driver.name != "vfio-pci") {
    throw std::runtime_error("device must be bound to vfio-pci; current driver=" +
                             driver.name);
  }
}

static IommuGroupInfo iommu_group_for_device(const fs::path &dev_dir) {
  fs::path group_link = dev_dir / "iommu_group";
  if (!fs::exists(group_link)) {
    throw std::runtime_error(dev_dir.string() +
                             " does not have an iommu_group symlink");
  }

  fs::path target = fs::read_symlink(group_link);
  fs::path group_path =
      target.is_absolute()
          ? target
          : fs::weakly_canonical(group_link.parent_path() / target);

  return {.id = group_path.filename().string(), .path = group_path};
}

static std::vector<fs::path> group_devices(const IommuGroupInfo &group) {
  std::vector<fs::path> devices;
  fs::path devices_dir = group.path / "devices";
  if (!fs::exists(devices_dir))
    return devices;

  for (const fs::directory_entry &entry : fs::directory_iterator(devices_dir)) {
    fs::path target = fs::read_symlink(entry.path());
    fs::path dev_path = target.is_absolute()
                            ? target
                            : fs::weakly_canonical(entry.path().parent_path() /
                                                   target);
    devices.push_back(dev_path);
  }

  std::sort(devices.begin(), devices.end());
  return devices;
}

static void print_group_devices(const IommuGroupInfo &group) {
  std::println("IOMMU group devices:");
  for (const fs::path &dev_path : group_devices(group)) {
    DriverInfo driver = current_driver(dev_path);
    std::println("  {}  driver={}", dev_path.filename().string(), driver.name);
  }
}

static VfioContext open_vfio_context(const std::string &bdf,
                                     const fs::path &dev_dir) {
  IommuGroupInfo group = iommu_group_for_device(dev_dir);
  std::println("IOMMU group: {} ({})", group.id, group.path.string());
  print_group_devices(group);

  VfioContext vfio;
  vfio.container = open_fd("/dev/vfio/vfio", O_RDWR);

  int api_version =
      ioctl_value(vfio.container.get(), VFIO_GET_API_VERSION,
                  "VFIO_GET_API_VERSION");
  if (api_version != VFIO_API_VERSION) {
    throw std::runtime_error("unsupported VFIO API version " +
                             std::to_string(api_version));
  }

  int type1 = ioctl_arg_value(vfio.container.get(), VFIO_CHECK_EXTENSION,
                              VFIO_TYPE1_IOMMU,
                              "VFIO_CHECK_EXTENSION(VFIO_TYPE1_IOMMU)");
  int type1v2 = ioctl_arg_value(vfio.container.get(), VFIO_CHECK_EXTENSION,
                                VFIO_TYPE1v2_IOMMU,
                                "VFIO_CHECK_EXTENSION(VFIO_TYPE1v2_IOMMU)");
  if (!type1 && !type1v2)
    throw std::runtime_error("VFIO Type1 IOMMU is not supported");

  vfio.group = open_fd(fs::path("/dev/vfio") / group.id, O_RDWR);

  vfio_group_status group_status{};
  group_status.argsz = sizeof(group_status);
  ioctl_checked(vfio.group.get(), VFIO_GROUP_GET_STATUS, &group_status,
                "VFIO_GROUP_GET_STATUS");
  if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    throw std::runtime_error(
        "IOMMU group is not viable; every device in the group must be bound "
        "to a VFIO-compatible driver or safely unbound");
  }

  int container_fd = vfio.container.get();
  ioctl_checked(vfio.group.get(), VFIO_GROUP_SET_CONTAINER, &container_fd,
                "VFIO_GROUP_SET_CONTAINER");

  int iommu_type = type1v2 ? VFIO_TYPE1v2_IOMMU : VFIO_TYPE1_IOMMU;
  ioctl_arg_value(vfio.container.get(), VFIO_SET_IOMMU, iommu_type,
                  "VFIO_SET_IOMMU");
  std::println("container IOMMU type: {}",
               iommu_type == VFIO_TYPE1v2_IOMMU ? "VFIO_TYPE1v2_IOMMU"
                                                 : "VFIO_TYPE1_IOMMU");

  int raw_device_fd = ::ioctl(vfio.group.get(), VFIO_GROUP_GET_DEVICE_FD,
                              bdf.c_str());
  if (raw_device_fd == -1) {
    int error = errno;
    throw std::runtime_error("VFIO_GROUP_GET_DEVICE_FD: " +
                             errno_text(error));
  }
  vfio.device = UniqueFd(raw_device_fd);

  return vfio;
}

static vfio_device_info get_device_info(int device_fd) {
  vfio_device_info info{};
  info.argsz = sizeof(info);
  ioctl_checked(device_fd, VFIO_DEVICE_GET_INFO, &info,
                "VFIO_DEVICE_GET_INFO");
  return info;
}

static vfio_irq_info get_irq_info(int device_fd, __u32 index) {
  vfio_irq_info irq{};
  irq.argsz = sizeof(irq);
  irq.index = index;
  ioctl_checked(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq,
                "VFIO_DEVICE_GET_IRQ_INFO");
  return irq;
}

static void print_irq_info(int device_fd, __u32 index) {
  vfio_irq_info irq{};
  irq.argsz = sizeof(irq);
  irq.index = index;

  if (::ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq) == -1) {
    int error = errno;
    if (error == EINVAL) {
      std::println("  irq {} ({})", index, irq_name(index));
      std::println("    unavailable: {}", errno_text(error));
      return;
    }
    throw std::runtime_error("VFIO_DEVICE_GET_IRQ_INFO: " +
                             errno_text(error));
  }

  std::println("  irq {} ({})", index, irq_name(index));
  std::println("    count: {}", irq.count);
  std::println("    flags: {}", irq_flag_names(irq.flags));
}

static void show_irqs(const std::string &bdf, const fs::path &dev_dir) {
  require_vfio_driver(dev_dir);

  std::println("BDF: {}", bdf);
  VfioContext vfio = open_vfio_context(bdf, dev_dir);

  vfio_device_info device_info = get_device_info(vfio.device.get());
  std::println("num_irqs: {}", device_info.num_irqs);
  std::println("IRQs:");
  for (__u32 index = 0; index < device_info.num_irqs; ++index)
    print_irq_info(vfio.device.get(), index);
}

static void set_irq_eventfd(int device_fd, __u32 index, __u32 vector,
                            std::int32_t event_fd) {
  std::vector<std::uint8_t> storage(sizeof(vfio_irq_set) + sizeof(event_fd));
  auto *irq_set = reinterpret_cast<vfio_irq_set *>(storage.data());
  irq_set->argsz = static_cast<__u32>(storage.size());
  irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
  irq_set->index = index;
  irq_set->start = vector;
  irq_set->count = 1;
  std::memcpy(irq_set->data, &event_fd, sizeof(event_fd));

  ioctl_checked(device_fd, VFIO_DEVICE_SET_IRQS, irq_set,
                "VFIO_DEVICE_SET_IRQS(eventfd trigger)");
}

static void trigger_irq_loopback(int device_fd, __u32 index, __u32 vector) {
  std::vector<std::uint8_t> storage(sizeof(vfio_irq_set));
  auto *irq_set = reinterpret_cast<vfio_irq_set *>(storage.data());
  irq_set->argsz = static_cast<__u32>(storage.size());
  irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
  irq_set->index = index;
  irq_set->start = vector;
  irq_set->count = 1;

  ioctl_checked(device_fd, VFIO_DEVICE_SET_IRQS, irq_set,
                "VFIO_DEVICE_SET_IRQS(loopback trigger)");
}

static std::uint64_t wait_for_eventfd(int event_fd,
                                      unsigned long long wait_ms) {
  int timeout = wait_ms > static_cast<unsigned long long>(
                              std::numeric_limits<int>::max())
                    ? std::numeric_limits<int>::max()
                    : static_cast<int>(wait_ms);

  pollfd pfd{};
  pfd.fd = event_fd;
  pfd.events = POLLIN;

  for (;;) {
    int ready = ::poll(&pfd, 1, timeout);
    if (ready == -1 && errno == EINTR)
      continue;
    if (ready == -1) {
      int error = errno;
      throw std::runtime_error("poll(eventfd): " + errno_text(error));
    }
    if (ready == 0)
      throw std::runtime_error("timed out waiting for eventfd");
    break;
  }

  std::uint64_t count = 0;
  ssize_t got = ::read(event_fd, &count, sizeof(count));
  if (got == -1) {
    int error = errno;
    throw std::runtime_error("read(eventfd): " + errno_text(error));
  }
  if (got != sizeof(count))
    throw std::runtime_error("short read from eventfd");
  return count;
}

static void wait_for_irq(const std::string &bdf, const fs::path &dev_dir,
                         __u32 irq_index, __u32 vector,
                         unsigned long long wait_ms, bool trigger_test,
                         bool yes) {
  require_vfio_driver(dev_dir);

  std::println("BDF: {}", bdf);
  std::println("IRQ index: {} ({})", irq_index, irq_name(irq_index));
  std::println("vector: {}", vector);
  std::println("wait-ms: {}", wait_ms);
  std::println("trigger-test: {}", trigger_test ? "yes" : "no");

  if (!yes) {
    std::println("Would open VFIO container/group/device");
    std::println("Would bind IRQ index {} vector {} to an eventfd", irq_index,
                 vector);
    if (trigger_test)
      std::println("Would ask VFIO to trigger a loopback interrupt event");
    std::println("Would poll the eventfd");
    std::println("Pass --yes to perform this IRQ eventfd test.");
    return;
  }

  VfioContext vfio = open_vfio_context(bdf, dev_dir);

  vfio_device_info device_info = get_device_info(vfio.device.get());
  if (irq_index >= device_info.num_irqs) {
    throw std::runtime_error("IRQ index " + std::to_string(irq_index) +
                             " is outside num_irqs=" +
                             std::to_string(device_info.num_irqs));
  }

  vfio_irq_info irq = get_irq_info(vfio.device.get(), irq_index);
  std::println("IRQ count: {}", irq.count);
  std::println("IRQ flags: {}", irq_flag_names(irq.flags));

  if (!(irq.flags & VFIO_IRQ_INFO_EVENTFD)) {
    throw std::runtime_error("IRQ index " + std::to_string(irq_index) +
                             " does not support eventfd delivery");
  }
  if (vector >= irq.count) {
    throw std::runtime_error("vector " + std::to_string(vector) +
                             " is outside IRQ count=" +
                             std::to_string(irq.count));
  }

  int raw_event_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (raw_event_fd == -1) {
    int error = errno;
    throw std::runtime_error("eventfd: " + errno_text(error));
  }
  UniqueFd event(raw_event_fd);

  set_irq_eventfd(vfio.device.get(), irq_index, vector, event.get());
  std::println("Bound IRQ index {} vector {} to eventfd {}", irq_index, vector,
               event.get());

  try {
    if (trigger_test) {
      trigger_irq_loopback(vfio.device.get(), irq_index, vector);
      std::println("Triggered VFIO loopback event");
    }

    std::uint64_t count = wait_for_eventfd(event.get(), wait_ms);
    std::println("eventfd signaled; counter={}", count);
  } catch (...) {
    set_irq_eventfd(vfio.device.get(), irq_index, vector, -1);
    std::println("Disabled IRQ eventfd binding");
    throw;
  }

  set_irq_eventfd(vfio.device.get(), irq_index, vector, -1);
  std::println("Disabled IRQ eventfd binding");
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  try {
    std::string bdf = normalize_bdf(argv[1]);
    fs::path dev_dir = fs::path("/sys/bus/pci/devices") / bdf;
    if (!fs::exists(dev_dir)) {
      std::println(std::cerr, "Error: PCI device {} does not exist", bdf);
      return 1;
    }

    bool show = false;
    bool yes = false;
    bool trigger_test = false;
    bool irq_requested = false;
    __u32 irq_index = 0;
    __u32 vector = 0;
    unsigned long long wait_ms = 5000;

    for (int i = 2; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "--show") {
        show = true;
      } else if (arg == "--irq" && i + 1 < argc) {
        irq_requested = true;
        irq_index = parse_u32(argv[++i], "--irq");
      } else if (arg == "--vector" && i + 1 < argc) {
        vector = parse_u32(argv[++i], "--vector");
      } else if (arg == "--wait-ms" && i + 1 < argc) {
        wait_ms = parse_ull(argv[++i], "--wait-ms");
      } else if (arg == "--trigger-test") {
        trigger_test = true;
      } else if (arg == "--dry-run") {
        yes = false;
      } else if (arg == "--yes") {
        yes = true;
      } else {
        usage(argv[0]);
        return 1;
      }
    }

    if (show && (irq_requested || yes || trigger_test)) {
      usage(argv[0]);
      return 1;
    }

    if (show) {
      show_irqs(bdf, dev_dir);
      return 0;
    }

    if (!irq_requested) {
      usage(argv[0]);
      return 1;
    }

    wait_for_irq(bdf, dev_dir, irq_index, vector, wait_ms, trigger_test, yes);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
