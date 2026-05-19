// Exercise 06: VFIO Device Info
//
// Goal: inspect a vfio-pci device through the VFIO userspace API.
//
// Background
// ----------
// Once a PCI device is bound to vfio-pci, userspace should stop accessing
// sysfs resource<N> files directly.  VFIO exposes the device through:
//
//   /dev/vfio/vfio
//   /dev/vfio/<iommu_group_id>
//
// This sample opens the VFIO container, group, and device, then prints device
// info, region info, and IRQ info.  It does not map DMA, mmap BARs, configure
// interrupts, or write device registers.
//
// New concepts
// ------------
// - VFIO container, IOMMU group, and device fd
// - VFIO group viability
// - VFIO device regions for BAR/config/ROM
// - VFIO IRQ indexes for INTx/MSI/MSI-X

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/vfio.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
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
               "  {} <BDF> --show\n\n"
               "Example:\n"
               "  {} c1:00.3 --show",
               argv0, argv0);
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

static UniqueFd open_fd(const fs::path &path, int flags) {
  int fd = ::open(path.c_str(), flags | O_CLOEXEC);
  if (fd == -1) {
    int error = errno;
    throw std::runtime_error("cannot open " + path.string() + ": " +
                             errno_text(error));
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

static std::string device_flag_names(__u32 flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_DEVICE_FLAGS_RESET)
    names.push_back("RESET");
  if (flags & VFIO_DEVICE_FLAGS_PCI)
    names.push_back("PCI");
  if (flags & VFIO_DEVICE_FLAGS_PLATFORM)
    names.push_back("PLATFORM");
  if (flags & VFIO_DEVICE_FLAGS_AMBA)
    names.push_back("AMBA");
  if (flags & VFIO_DEVICE_FLAGS_CCW)
    names.push_back("CCW");
  if (flags & VFIO_DEVICE_FLAGS_AP)
    names.push_back("AP");
  if (flags & VFIO_DEVICE_FLAGS_FSL_MC)
    names.push_back("FSL_MC");
  if (flags & VFIO_DEVICE_FLAGS_CAPS)
    names.push_back("CAPS");
#ifdef VFIO_DEVICE_FLAGS_CDX
  if (flags & VFIO_DEVICE_FLAGS_CDX)
    names.push_back("CDX");
#endif
  return join_flags(names);
}

static std::string group_flag_names(__u32 flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_GROUP_FLAGS_VIABLE)
    names.push_back("VIABLE");
  if (flags & VFIO_GROUP_FLAGS_CONTAINER_SET)
    names.push_back("CONTAINER_SET");
  return join_flags(names);
}

static std::string region_flag_names(__u32 flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_REGION_INFO_FLAG_READ)
    names.push_back("READ");
  if (flags & VFIO_REGION_INFO_FLAG_WRITE)
    names.push_back("WRITE");
  if (flags & VFIO_REGION_INFO_FLAG_MMAP)
    names.push_back("MMAP");
  if (flags & VFIO_REGION_INFO_FLAG_CAPS)
    names.push_back("CAPS");
  return join_flags(names);
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

static std::string region_name(__u32 index) {
  switch (index) {
  case VFIO_PCI_BAR0_REGION_INDEX:
    return "BAR0";
  case VFIO_PCI_BAR1_REGION_INDEX:
    return "BAR1";
  case VFIO_PCI_BAR2_REGION_INDEX:
    return "BAR2";
  case VFIO_PCI_BAR3_REGION_INDEX:
    return "BAR3";
  case VFIO_PCI_BAR4_REGION_INDEX:
    return "BAR4";
  case VFIO_PCI_BAR5_REGION_INDEX:
    return "BAR5";
  case VFIO_PCI_ROM_REGION_INDEX:
    return "ROM";
  case VFIO_PCI_CONFIG_REGION_INDEX:
    return "CONFIG";
  case VFIO_PCI_VGA_REGION_INDEX:
    return "VGA";
  default:
    return "device-specific";
  }
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

static void print_group_devices(const IommuGroupInfo &group) {
  std::println("IOMMU group devices:");
  for (const fs::path &dev_path : group_devices(group)) {
    DriverInfo driver = current_driver(dev_path);
    std::println("  {}  driver={}", dev_path.filename().string(), driver.name);
  }
}

static void print_region_info(int device_fd, __u32 index) {
  vfio_region_info region{};
  region.argsz = sizeof(region);
  region.index = index;

  if (::ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region) == -1) {
    int error = errno;
    if (error == EINVAL) {
      std::println("  region {} ({})", index, region_name(index));
      std::println("    unavailable: {}", errno_text(error));
      return;
    }
    throw std::runtime_error("VFIO_DEVICE_GET_REGION_INFO: " +
                             errno_text(error));
  }

  std::println("  region {} ({})", index, region_name(index));
  std::println("    size: 0x{:x} ({})", region.size, region.size);
  std::println("    offset: 0x{:x}", region.offset);
  std::println("    flags: {}", region_flag_names(region.flags));
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

static void show_vfio_device(const std::string &bdf, const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);
  IommuGroupInfo group = iommu_group_for_device(dev_dir);

  std::println("BDF: {}", bdf);
  std::println("device path: {}", dev_dir.string());
  std::println("driver: {}", driver.name);
  if (driver.bound)
    std::println("driver path: {}", driver.path.string());
  std::println("IOMMU group: {} ({})", group.id, group.path.string());
  print_group_devices(group);

  UniqueFd container = open_fd("/dev/vfio/vfio", O_RDWR);

  int api_version =
      ioctl_value(container.get(), VFIO_GET_API_VERSION,
                  "VFIO_GET_API_VERSION");
  std::println("VFIO API version: {}", api_version);
  if (api_version != VFIO_API_VERSION) {
    throw std::runtime_error("unsupported VFIO API version " +
                             std::to_string(api_version));
  }

  int type1 =
      ioctl_arg_value(container.get(), VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU,
                      "VFIO_CHECK_EXTENSION(VFIO_TYPE1_IOMMU)");
  int type1v2 =
      ioctl_arg_value(container.get(), VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU,
                      "VFIO_CHECK_EXTENSION(VFIO_TYPE1v2_IOMMU)");
  std::println("VFIO_TYPE1_IOMMU: {}", type1 ? "supported" : "unsupported");
  std::println("VFIO_TYPE1v2_IOMMU: {}",
               type1v2 ? "supported" : "unsupported");
  if (!type1 && !type1v2)
    throw std::runtime_error("VFIO Type1 IOMMU is not supported");

  UniqueFd group_fd = open_fd(fs::path("/dev/vfio") / group.id, O_RDWR);

  vfio_group_status group_status{};
  group_status.argsz = sizeof(group_status);
  ioctl_checked(group_fd.get(), VFIO_GROUP_GET_STATUS, &group_status,
                "VFIO_GROUP_GET_STATUS");
  std::println("group flags: {}", group_flag_names(group_status.flags));
  if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    throw std::runtime_error(
        "IOMMU group is not viable; every device in the group must be bound "
        "to a VFIO-compatible driver or safely unbound");
  }

  int container_fd = container.get();
  ioctl_checked(group_fd.get(), VFIO_GROUP_SET_CONTAINER, &container_fd,
                "VFIO_GROUP_SET_CONTAINER");

  int iommu_type = type1v2 ? VFIO_TYPE1v2_IOMMU : VFIO_TYPE1_IOMMU;
  ioctl_arg_value(container.get(), VFIO_SET_IOMMU, iommu_type,
                  "VFIO_SET_IOMMU");
  std::println("container IOMMU type: {}",
               iommu_type == VFIO_TYPE1v2_IOMMU ? "VFIO_TYPE1v2_IOMMU"
                                                 : "VFIO_TYPE1_IOMMU");

  int raw_device_fd = ::ioctl(group_fd.get(), VFIO_GROUP_GET_DEVICE_FD,
                              bdf.c_str());
  if (raw_device_fd == -1) {
    int error = errno;
    throw std::runtime_error("VFIO_GROUP_GET_DEVICE_FD: " +
                             errno_text(error));
  }
  UniqueFd device_fd(raw_device_fd);

  vfio_device_info device_info{};
  device_info.argsz = sizeof(device_info);
  ioctl_checked(device_fd.get(), VFIO_DEVICE_GET_INFO, &device_info,
                "VFIO_DEVICE_GET_INFO");

  std::println("device flags: {}", device_flag_names(device_info.flags));
  std::println("num_regions: {}", device_info.num_regions);
  std::println("num_irqs: {}", device_info.num_irqs);

  std::println("Regions:");
  for (__u32 index = 0; index < device_info.num_regions; ++index)
    print_region_info(device_fd.get(), index);

  std::println("IRQs:");
  for (__u32 index = 0; index < device_info.num_irqs; ++index)
    print_irq_info(device_fd.get(), index);
}

int main(int argc, char *argv[]) {
  if (argc != 3 || std::string_view(argv[2]) != "--show") {
    usage(argv[0]);
    return 1;
  }

  std::string bdf = normalize_bdf(argv[1]);
  fs::path dev_dir = fs::path("/sys/bus/pci/devices") / bdf;

  if (!fs::exists(dev_dir)) {
    std::println(std::cerr, "Error: PCI device {} does not exist", bdf);
    return 1;
  }

  try {
    show_vfio_device(bdf, dev_dir);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
