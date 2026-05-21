// Exercise 07: VFIO Region Dump
//
// Goal: mmap a VFIO PCI region and dump bytes from it.
//
// Background
// ----------
// Exercise 03 used sysfs resource<N> files to mmap a BAR.  That works for
// unbound devices, but once a device is bound to vfio-pci, userspace should
// access BARs through the VFIO device fd and VFIO region offsets.
//
// This sample opens the VFIO container, group, and device, queries a region,
// mmap()s it if supported, and reads bytes through a volatile pointer.  It does
// not write BARs, configure DMA, or enable interrupts.
//
// New concepts
// ------------
// - VFIO region offset
// - mmap() on a VFIO device fd
// - Page-aligning region-relative offsets for mmap()
// - BAR reads through VFIO instead of sysfs resource<N>

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

class MappedRegion {
public:
  MappedRegion(int fd, std::size_t size, off_t offset) : size_(size) {
    void *ptr = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, offset);
    if (ptr == MAP_FAILED) {
      int error = errno;
      throw std::runtime_error("mmap: " + errno_text(error));
    }
    base_ = static_cast<volatile std::uint8_t *>(ptr);
  }

  MappedRegion(const MappedRegion &) = delete;
  MappedRegion &operator=(const MappedRegion &) = delete;

  ~MappedRegion() {
    if (base_)
      ::munmap(const_cast<std::uint8_t *>(base_), size_);
  }

  std::uint8_t read8(std::size_t offset) const {
    if (offset >= size_)
      throw std::out_of_range("MMIO read past mapped range");
    return *(base_ + offset);
  }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  volatile std::uint8_t *base_ = nullptr;
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

struct VfioDevice {
  UniqueFd container;
  UniqueFd group;
  UniqueFd device;
};

struct MmapArea {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;

  bool contains(std::uint64_t range_offset, std::uint64_t range_size) const {
    return range_offset >= offset &&
           range_offset - offset <= size &&
           range_size <= size - (range_offset - offset);
  }
};

struct RegionInfo {
  vfio_region_info info{};
  std::vector<MmapArea> mmap_areas;
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
               "  {} <BDF> <region_index> [offset] [length]\n\n"
               "Examples:\n"
               "  {} c1:00.3 4 0 64\n"
               "  {} 0000:c1:00.3 4 0x100 128",
               argv0, argv0, argv0);
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

static VfioDevice open_vfio_device(const std::string &bdf,
                                   const fs::path &dev_dir) {
  IommuGroupInfo group = iommu_group_for_device(dev_dir);
  std::println("IOMMU group: {} ({})", group.id, group.path.string());
  print_group_devices(group);

  VfioDevice vfio;
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

static void parse_region_caps(RegionInfo &region, const std::vector<char> &buf) {
  if (!(region.info.flags & VFIO_REGION_INFO_FLAG_CAPS))
    return;

  std::uint32_t offset = region.info.cap_offset;
  while (offset != 0) {
    if (offset + sizeof(vfio_info_cap_header) > buf.size())
      throw std::runtime_error("VFIO region capability chain is truncated");

    const auto *header =
        reinterpret_cast<const vfio_info_cap_header *>(buf.data() + offset);

    if (header->id == VFIO_REGION_INFO_CAP_SPARSE_MMAP) {
      if (offset + sizeof(vfio_region_info_cap_sparse_mmap) > buf.size()) {
        throw std::runtime_error("VFIO sparse mmap capability is truncated");
      }

      const auto *sparse =
          reinterpret_cast<const vfio_region_info_cap_sparse_mmap *>(
              buf.data() + offset);
      std::size_t bytes =
          sizeof(vfio_region_info_cap_sparse_mmap) +
          static_cast<std::size_t>(sparse->nr_areas) *
              sizeof(vfio_region_sparse_mmap_area);
      if (offset + bytes > buf.size()) {
        throw std::runtime_error("VFIO sparse mmap area list is truncated");
      }

      for (std::uint32_t i = 0; i < sparse->nr_areas; ++i) {
        region.mmap_areas.push_back({
            .offset = sparse->areas[i].offset,
            .size = sparse->areas[i].size,
        });
      }
    }

    offset = header->next;
  }
}

static RegionInfo get_region_info(int device_fd, __u32 index) {
  vfio_region_info region{};
  region.argsz = sizeof(region);
  region.index = index;
  ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region,
                "VFIO_DEVICE_GET_REGION_INFO");

  RegionInfo result;
  result.info = region;

  if ((region.flags & VFIO_REGION_INFO_FLAG_CAPS) &&
      region.argsz > sizeof(vfio_region_info)) {
    std::vector<char> buf(region.argsz);
    auto *full = reinterpret_cast<vfio_region_info *>(buf.data());
    full->argsz = static_cast<__u32>(buf.size());
    full->index = index;
    ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, full,
                  "VFIO_DEVICE_GET_REGION_INFO");
    result.info = *full;
    parse_region_caps(result, buf);
  }

  return result;
}

static void hexdump(const MappedRegion &mapping, std::size_t display_offset,
                    std::size_t mapping_offset, std::size_t length) {
  for (std::size_t i = 0; i < length; i += 16) {
    std::print("{:08x} ", display_offset + i);

    for (std::size_t j = 0; j < 16; ++j) {
      if (i + j < length)
        std::print(" {:02x}", mapping.read8(mapping_offset + i + j));
      else
        std::print("   ");
    }

    std::print("  ");
    for (std::size_t j = 0; j < 16 && i + j < length; ++j) {
      std::uint8_t c = mapping.read8(mapping_offset + i + j);
      std::print("{}", (c >= 0x20 && c < 0x7f) ? char(c) : '.');
    }
    std::println("");
  }
}

static void dump_region(const std::string &bdf, __u32 region_index,
                        std::uint64_t offset, std::size_t length) {
  fs::path dev_dir = fs::path("/sys/bus/pci/devices") / bdf;
  if (!fs::exists(dev_dir))
    throw std::runtime_error("PCI device " + bdf + " does not exist");

  require_vfio_driver(dev_dir);
  VfioDevice vfio = open_vfio_device(bdf, dev_dir);
  RegionInfo region = get_region_info(vfio.device.get(), region_index);
  const vfio_region_info &info = region.info;

  if (info.size == 0) {
    throw std::runtime_error("region " + std::to_string(region_index) + " (" +
                             region_name(region_index) + ") is not present");
  }
  if (!(info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
    throw std::runtime_error("region " + std::to_string(region_index) + " (" +
                             region_name(region_index) +
                             ") does not support mmap");
  }
  if (offset >= info.size) {
    throw std::out_of_range("offset 0x" + std::to_string(offset) +
                            " is past region size");
  }

  std::size_t available = static_cast<std::size_t>(info.size - offset);
  std::size_t dump_length = std::min(length, available);
  if (dump_length == 0)
    throw std::runtime_error("length must be greater than 0");
  if (!region.mmap_areas.empty()) {
    auto area = std::ranges::find_if(region.mmap_areas, [&](const MmapArea &a) {
      return a.contains(offset, dump_length);
    });
    if (area == region.mmap_areas.end()) {
      throw std::runtime_error(
          "requested range is not fully inside a VFIO sparse mmap area");
    }
  }

  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("cannot determine system page size");

  std::uint64_t page_mask = static_cast<std::uint64_t>(page_size - 1);
  std::uint64_t aligned_offset = offset & ~page_mask;
  std::size_t mapping_delta = static_cast<std::size_t>(offset - aligned_offset);
  std::size_t mapping_size = mapping_delta + dump_length;
  off_t mmap_offset = static_cast<off_t>(info.offset + aligned_offset);

  MappedRegion mapping(vfio.device.get(), mapping_size, mmap_offset);

  std::println("Mapped VFIO region {} ({})", region_index,
               region_name(region_index));
  std::println("region size: 0x{:x} ({})", info.size, info.size);
  std::println("region offset: 0x{:x}", info.offset);
  if (!region.mmap_areas.empty()) {
    std::println("sparse mmap areas:");
    for (const MmapArea &area : region.mmap_areas) {
      std::println("  offset=0x{:x} size=0x{:x}", area.offset, area.size);
    }
  }
  std::println("mmap file offset: 0x{:x}", mmap_offset);
  std::println("Dumping {} bytes at region offset 0x{:x}:", dump_length,
               offset);
  hexdump(mapping, static_cast<std::size_t>(offset), mapping_delta,
          dump_length);
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  try {
    std::string bdf = normalize_bdf(argv[1]);
    auto region_index =
        static_cast<__u32>(parse_ull(argv[2], "region_index"));
    std::uint64_t offset =
        argc > 3 ? parse_ull(argv[3], "offset") : 0;
    std::size_t length =
        argc > 4 ? static_cast<std::size_t>(parse_ull(argv[4], "length"))
                 : 128;

    if (argc > 5) {
      usage(argv[0]);
      return 1;
    }

    dump_region(bdf, region_index, offset, length);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
