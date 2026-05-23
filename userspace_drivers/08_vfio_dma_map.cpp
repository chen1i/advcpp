// Exercise 08: VFIO DMA Map / Unmap
//
// Goal: map a userspace buffer into a VFIO IOMMU container.
//
// Background
// ----------
// A userspace driver cannot hand a normal virtual address directly to a PCI
// device.  The device speaks in IOVA (IO virtual address) terms.  VFIO bridges
// that gap:
//
//   userspace buffer virtual address -> VFIO_IOMMU_MAP_DMA -> IOVA
//
// This sample allocates an anonymous page-aligned buffer, maps it to an IOVA,
// then immediately unmaps it.  It does not tell the device about the IOVA,
// write BAR registers, or trigger DMA.
//
// New concepts
// ------------
// - IOVA vs userspace virtual address
// - VFIO_IOMMU_GET_INFO
// - VFIO_IOMMU_MAP_DMA
// - VFIO_IOMMU_UNMAP_DMA

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <linux/vfio.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

class AnonymousBuffer {
public:
  explicit AnonymousBuffer(std::size_t size) : size_(size) {
    void *ptr = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
      int error = errno;
      throw std::runtime_error("mmap anonymous buffer: " + errno_text(error));
    }
    data_ = static_cast<std::uint8_t *>(ptr);
  }

  AnonymousBuffer(const AnonymousBuffer &) = delete;
  AnonymousBuffer &operator=(const AnonymousBuffer &) = delete;

  ~AnonymousBuffer() {
    if (data_)
      ::munmap(data_, size_);
  }

  std::uint8_t *data() const { return data_; }
  std::size_t size() const { return size_; }

  void fill_pattern() {
    for (std::size_t i = 0; i < size_; ++i)
      data_[i] = static_cast<std::uint8_t>(i & 0xff);
  }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  std::uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
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
               "  {} <BDF> [iova] [size] [--read] [--write] [--yes]\n\n"
               "Defaults:\n"
               "  iova = 0x100000000\n"
               "  size = 4096\n"
               "  permissions = read|write\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.3\n"
               "  {} c1:00.3 0x100000000 4096 --yes\n"
               "  {} c1:00.3 0x200000000 0x2000 --read --write --yes",
               argv0, argv0, argv0, argv0);
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

static std::string dma_flag_names(std::uint32_t flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_DMA_MAP_FLAG_READ)
    names.push_back("READ");
  if (flags & VFIO_DMA_MAP_FLAG_WRITE)
    names.push_back("WRITE");

  if (names.empty())
    return "none";

  std::string out;
  for (std::string_view name : names) {
    if (!out.empty())
      out += "|";
    out += name;
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

static void print_group_devices(const IommuGroupInfo &group) {
  std::println("IOMMU group devices:");
  for (const fs::path &dev_path : group_devices(group)) {
    DriverInfo driver = current_driver(dev_path);
    std::println("  {}  driver={}", dev_path.filename().string(), driver.name);
  }
}

static void require_vfio_driver(const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);
  if (driver.name != "vfio-pci") {
    throw std::runtime_error("device must be bound to vfio-pci; current driver=" +
                             driver.name);
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

static void print_iommu_info(int container_fd) {
  vfio_iommu_type1_info info{};
  info.argsz = sizeof(info);
  ioctl_checked(container_fd, VFIO_IOMMU_GET_INFO, &info,
                "VFIO_IOMMU_GET_INFO");

  if (info.flags & VFIO_IOMMU_INFO_PGSIZES)
    std::println("IOMMU page-size bitmap: 0x{:x}", info.iova_pgsizes);
  else
    std::println("IOMMU page-size bitmap: <not reported>");
}

static std::size_t round_up_to_page(std::size_t size) {
  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("cannot determine system page size");

  std::size_t page = static_cast<std::size_t>(page_size);
  if (size == 0)
    throw std::runtime_error("size must be greater than 0");
  return ((size + page - 1) / page) * page;
}

static void dma_map_test(const std::string &bdf, std::uint64_t iova,
                         std::size_t requested_size, std::uint32_t flags,
                         bool yes) {
  fs::path dev_dir = fs::path("/sys/bus/pci/devices") / bdf;
  if (!fs::exists(dev_dir))
    throw std::runtime_error("PCI device " + bdf + " does not exist");

  if ((flags & (VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE)) == 0) {
    throw std::runtime_error("at least one of --read or --write is required");
  }

  std::size_t size = round_up_to_page(requested_size);
  require_vfio_driver(dev_dir);

  std::println("BDF: {}", bdf);
  std::println("IOVA: 0x{:x}", iova);
  std::println("requested size: {}", requested_size);
  std::println("page-rounded size: {}", size);
  std::println("DMA permissions: {}", dma_flag_names(flags));

  if (!yes) {
    std::println("Would allocate a page-aligned userspace buffer");
    std::println("Would call VFIO_IOMMU_MAP_DMA");
    std::println("Would call VFIO_IOMMU_UNMAP_DMA");
    std::println("Pass --yes to perform this map/unmap test.");
    return;
  }

  VfioContext vfio = open_vfio_context(bdf, dev_dir);
  print_iommu_info(vfio.container.get());

  AnonymousBuffer buffer(size);
  buffer.fill_pattern();

  vfio_iommu_type1_dma_map map{};
  map.argsz = sizeof(map);
  map.flags = flags;
  map.vaddr = reinterpret_cast<std::uintptr_t>(buffer.data());
  map.iova = iova;
  map.size = size;

  ioctl_checked(vfio.container.get(), VFIO_IOMMU_MAP_DMA, &map,
                "VFIO_IOMMU_MAP_DMA");
  std::println("Mapped userspace buffer {:p} to IOVA 0x{:x} ({} bytes)",
               static_cast<void *>(buffer.data()), iova, size);

  vfio_iommu_type1_dma_unmap unmap{};
  unmap.argsz = sizeof(unmap);
  unmap.iova = iova;
  unmap.size = size;

  ioctl_checked(vfio.container.get(), VFIO_IOMMU_UNMAP_DMA, &unmap,
                "VFIO_IOMMU_UNMAP_DMA");
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped", iova,
               unmap.size);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  try {
    std::string bdf = normalize_bdf(argv[1]);
    std::uint64_t iova = 0x100000000ull;
    std::size_t size = 4096;
    bool yes = false;
    bool saw_permission = false;
    std::uint32_t flags = 0;

    int index = 2;
    if (index < argc && std::string_view(argv[index]).starts_with("--") == false)
      iova = parse_ull(argv[index++], "iova");
    if (index < argc && std::string_view(argv[index]).starts_with("--") == false)
      size = static_cast<std::size_t>(parse_ull(argv[index++], "size"));

    for (; index < argc; ++index) {
      std::string_view arg = argv[index];
      if (arg == "--read") {
        flags |= VFIO_DMA_MAP_FLAG_READ;
        saw_permission = true;
      } else if (arg == "--write") {
        flags |= VFIO_DMA_MAP_FLAG_WRITE;
        saw_permission = true;
      } else if (arg == "--dry-run") {
        yes = false;
      } else if (arg == "--yes") {
        yes = true;
      } else {
        usage(argv[0]);
        return 1;
      }
    }

    if (!saw_permission)
      flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

    dma_map_test(bdf, iova, size, flags, yes);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
