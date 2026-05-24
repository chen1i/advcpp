// Exercise 12: Virtio queue info through VFIO
//
// Goal: select virtqueues through COMMON_CFG and inspect their state.
//
// Background
// ----------
// The virtio common configuration exposes queue fields as a selected-queue
// window.  A driver writes queue_select, then reads fields such as queue_size,
// queue_enable, queue_notify_off, and the descriptor/avail/used addresses:
//
//   write queue_select = N -> read queue_size / queue_enable / queue_notify_off
//
// This sample only writes queue_select.  It restores the original selection
// before exiting.  It does not write queue addresses, enable queues, notify the
// device, negotiate features, or start DMA.
//
// New concepts
// ------------
// - queue_select as a selected-queue window
// - queue_size and queue_notify_off
// - queue_desc / queue_avail / queue_used address registers
// - Restoring device register selection after inspection

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <linux/pci_regs.h>
#include <linux/vfio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static_assert(std::endian::native == std::endian::little,
              "this tutorial sample expects a little-endian host");

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
  MappedRegion(int fd, std::size_t size, off_t offset, bool writable)
      : size_(size) {
    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *ptr = ::mmap(nullptr, size_, prot, MAP_SHARED, fd, offset);
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
    check_range(offset, sizeof(std::uint8_t));
    return *(base_ + offset);
  }

  std::uint16_t read16(std::size_t offset) const {
    check_range(offset, sizeof(std::uint16_t));
    return *reinterpret_cast<volatile const std::uint16_t *>(base_ + offset);
  }

  std::uint32_t read32(std::size_t offset) const {
    check_range(offset, sizeof(std::uint32_t));
    return *reinterpret_cast<volatile const std::uint32_t *>(base_ + offset);
  }

  void write16(std::size_t offset, std::uint16_t value) {
    check_range(offset, sizeof(std::uint16_t));
    *reinterpret_cast<volatile std::uint16_t *>(base_ + offset) = value;
  }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  void check_range(std::size_t offset, std::size_t width) const {
    if (offset > size_ || width > size_ - offset)
      throw std::out_of_range("MMIO access past mapped range");
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

struct VfioContext {
  UniqueFd container;
  UniqueFd group;
  UniqueFd device;
};

struct ConfigRegion {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct MmapArea {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;

  bool contains(std::uint64_t range_offset, std::uint64_t range_size) const {
    return range_offset >= offset && range_offset - offset <= size &&
           range_size <= size - (range_offset - offset);
  }
};

struct RegionInfo {
  vfio_region_info info{};
  std::vector<MmapArea> mmap_areas;
};

struct VirtioCap {
  std::uint8_t cfg_type = 0;
  std::uint8_t bar = 0;
  std::uint8_t id = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
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
               "  {} <BDF> --show [--queue <n>] [--dry-run|--yes]\n\n"
               "Defaults:\n"
               "  queue = all queues\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.6 --show\n"
               "  {} c1:00.6 --show --queue 0 --yes\n"
               "  {} c1:00.6 --show --yes",
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

static std::uint16_t parse_u16(std::string_view text, std::string_view name) {
  unsigned long long value = parse_ull(text, name);
  if (value > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }
  return static_cast<std::uint16_t>(value);
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

static std::string status_names(std::uint8_t status) {
  std::vector<std::string_view> names;
  if (status & VIRTIO_CONFIG_S_ACKNOWLEDGE)
    names.push_back("ACKNOWLEDGE");
  if (status & VIRTIO_CONFIG_S_DRIVER)
    names.push_back("DRIVER");
  if (status & VIRTIO_CONFIG_S_DRIVER_OK)
    names.push_back("DRIVER_OK");
  if (status & VIRTIO_CONFIG_S_FEATURES_OK)
    names.push_back("FEATURES_OK");
  if (status & VIRTIO_CONFIG_S_NEEDS_RESET)
    names.push_back("NEEDS_RESET");
  if (status & VIRTIO_CONFIG_S_FAILED)
    names.push_back("FAILED");
  return join_flags(names);
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

static ConfigRegion get_config_region(int device_fd) {
  vfio_region_info region{};
  region.argsz = sizeof(region);
  region.index = VFIO_PCI_CONFIG_REGION_INDEX;
  ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region,
                "VFIO_DEVICE_GET_REGION_INFO(CONFIG)");

  return {.offset = region.offset, .size = region.size};
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
      if (offset + sizeof(vfio_region_info_cap_sparse_mmap) > buf.size())
        throw std::runtime_error("VFIO sparse mmap capability is truncated");

      const auto *sparse =
          reinterpret_cast<const vfio_region_info_cap_sparse_mmap *>(
              buf.data() + offset);
      std::size_t bytes =
          sizeof(vfio_region_info_cap_sparse_mmap) +
          static_cast<std::size_t>(sparse->nr_areas) *
              sizeof(vfio_region_sparse_mmap_area);
      if (offset + bytes > buf.size())
        throw std::runtime_error("VFIO sparse mmap area list is truncated");

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

static void pread_exact(int fd, void *data, std::size_t size,
                        std::uint64_t offset, std::string_view what) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    throw std::runtime_error(std::string(what) + ": offset too large");

  auto *out = static_cast<std::uint8_t *>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t got = ::pread(fd, out + done, size - done,
                          static_cast<off_t>(offset + done));
    if (got == -1 && errno == EINTR)
      continue;
    if (got == -1) {
      int error = errno;
      throw std::runtime_error(std::string(what) + ": " + errno_text(error));
    }
    if (got == 0)
      throw std::runtime_error(std::string(what) + ": short read");
    done += static_cast<std::size_t>(got);
  }
}

static void read_config_bytes(int device_fd, const ConfigRegion &config,
                              std::uint16_t offset, void *data,
                              std::size_t size) {
  if (static_cast<std::uint64_t>(offset) + size > config.size)
    throw std::runtime_error("PCI config read past VFIO CONFIG region");
  pread_exact(device_fd, data, size, config.offset + offset, "pread CONFIG");
}

static std::uint8_t read_config_u8(int device_fd, const ConfigRegion &config,
                                   std::uint16_t offset) {
  std::uint8_t value = 0;
  read_config_bytes(device_fd, config, offset, &value, sizeof(value));
  return value;
}

static std::uint16_t read_config_le16(int device_fd, const ConfigRegion &config,
                                      std::uint16_t offset) {
  std::uint8_t bytes[2]{};
  read_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

static std::uint32_t read_config_le32(int device_fd, const ConfigRegion &config,
                                      std::uint16_t offset) {
  std::uint8_t bytes[4]{};
  read_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

static std::uint64_t read_config_le64_from_halves(int device_fd,
                                                  const ConfigRegion &config,
                                                  std::uint16_t lo_offset,
                                                  std::uint16_t hi_offset) {
  std::uint64_t lo = read_config_le32(device_fd, config, lo_offset);
  std::uint64_t hi = read_config_le32(device_fd, config, hi_offset);
  return lo | (hi << 32);
}

static std::vector<VirtioCap> read_virtio_caps(int device_fd,
                                               const ConfigRegion &config) {
  std::uint16_t status = read_config_le16(device_fd, config, PCI_STATUS);
  if (!(status & PCI_STATUS_CAP_LIST))
    return {};

  std::vector<VirtioCap> caps;
  std::vector<bool> visited(256, false);
  std::uint8_t cap = read_config_u8(device_fd, config, PCI_CAPABILITY_LIST);

  for (unsigned hop = 0; cap != 0 && hop < 64; ++hop) {
    cap &= ~0x3u;
    if (cap < 0x40 || static_cast<std::uint64_t>(cap) + 2 > config.size)
      break;
    if (visited[cap])
      break;
    visited[cap] = true;

    std::uint8_t cap_id = read_config_u8(device_fd, config,
                                         cap + PCI_CAP_LIST_ID);
    std::uint8_t next = read_config_u8(device_fd, config,
                                       cap + PCI_CAP_LIST_NEXT);

    if (cap_id == PCI_CAP_ID_VNDR) {
      std::uint8_t cap_len = read_config_u8(device_fd, config, cap + 2);
      if (cap_len >= sizeof(virtio_pci_cap) &&
          static_cast<std::uint64_t>(cap) + cap_len <= config.size) {
        VirtioCap virtio;
        virtio.cfg_type =
            read_config_u8(device_fd, config,
                           cap + VIRTIO_PCI_CAP_CFG_TYPE);
        virtio.bar =
            read_config_u8(device_fd, config, cap + VIRTIO_PCI_CAP_BAR);
        virtio.id = read_config_u8(device_fd, config, cap + 5);
        virtio.offset =
            read_config_le32(device_fd, config, cap + VIRTIO_PCI_CAP_OFFSET);
        virtio.length =
            read_config_le32(device_fd, config, cap + VIRTIO_PCI_CAP_LENGTH);

        if (cap_len >= sizeof(virtio_pci_cap64)) {
          virtio.offset = read_config_le64_from_halves(
              device_fd, config, cap + VIRTIO_PCI_CAP_OFFSET,
              cap + sizeof(virtio_pci_cap));
          virtio.length = read_config_le64_from_halves(
              device_fd, config, cap + VIRTIO_PCI_CAP_LENGTH,
              cap + sizeof(virtio_pci_cap) + sizeof(std::uint32_t));
        }

        caps.push_back(virtio);
      }
    }

    cap = next;
  }

  return caps;
}

static VirtioCap find_common_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
      return cap;
  }
  throw std::runtime_error("virtio COMMON_CFG capability was not found");
}

static __u32 vfio_bar_region_index(std::uint8_t bar) {
  if (bar > 5)
    throw std::runtime_error("virtio capability references invalid BAR " +
                             std::to_string(bar));
  return VFIO_PCI_BAR0_REGION_INDEX + bar;
}

static std::uint64_t combine_u64(std::uint32_t lo, std::uint32_t hi) {
  return static_cast<std::uint64_t>(lo) |
         (static_cast<std::uint64_t>(hi) << 32);
}

static void print_queue_view(const MappedRegion &mapping,
                             std::size_t mapping_delta) {
  auto read16 = [&](std::size_t off) {
    return mapping.read16(mapping_delta + off);
  };
  auto read32 = [&](std::size_t off) {
    return mapping.read32(mapping_delta + off);
  };

  std::uint64_t desc = combine_u64(read32(VIRTIO_PCI_COMMON_Q_DESCLO),
                                   read32(VIRTIO_PCI_COMMON_Q_DESCHI));
  std::uint64_t avail = combine_u64(read32(VIRTIO_PCI_COMMON_Q_AVAILLO),
                                    read32(VIRTIO_PCI_COMMON_Q_AVAILHI));
  std::uint64_t used = combine_u64(read32(VIRTIO_PCI_COMMON_Q_USEDLO),
                                   read32(VIRTIO_PCI_COMMON_Q_USEDHI));

  std::println("    queue_size: {}", read16(VIRTIO_PCI_COMMON_Q_SIZE));
  std::println("    queue_msix_vector: 0x{:04x} ({})",
               read16(VIRTIO_PCI_COMMON_Q_MSIX),
               read16(VIRTIO_PCI_COMMON_Q_MSIX));
  std::println("    queue_enable: {}", read16(VIRTIO_PCI_COMMON_Q_ENABLE));
  std::println("    queue_notify_off: {}", read16(VIRTIO_PCI_COMMON_Q_NOFF));
  std::println("    queue_desc: 0x{:016x}", desc);
  std::println("    queue_avail: 0x{:016x}", avail);
  std::println("    queue_used: 0x{:016x}", used);
#ifdef VIRTIO_PCI_COMMON_Q_NDATA
  std::println("    queue_notify_data: {}",
               read16(VIRTIO_PCI_COMMON_Q_NDATA));
#endif
#ifdef VIRTIO_PCI_COMMON_Q_RESET
  std::println("    queue_reset: {}", read16(VIRTIO_PCI_COMMON_Q_RESET));
#endif
}

static void write_queue_select(MappedRegion &mapping, std::size_t mapping_delta,
                               std::uint16_t queue) {
  mapping.write16(mapping_delta + VIRTIO_PCI_COMMON_Q_SELECT, queue);
}

static void show_queue_info(const std::string &bdf, const fs::path &dev_dir,
                            std::optional<std::uint16_t> requested_queue,
                            bool yes) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);

  VfioContext vfio = open_vfio_context(bdf, dev_dir);
  ConfigRegion config = get_config_region(vfio.device.get());
  VirtioCap common = find_common_cfg(read_virtio_caps(vfio.device.get(), config));

  __u32 region_index = vfio_bar_region_index(common.bar);
  RegionInfo region = get_region_info(vfio.device.get(), region_index);
  const vfio_region_info &info = region.info;

  if (info.size == 0)
    throw std::runtime_error("COMMON_CFG BAR region is not present");
  if (!(info.flags & VFIO_REGION_INFO_FLAG_MMAP))
    throw std::runtime_error("COMMON_CFG BAR region does not support mmap");
  if (common.offset >= info.size)
    throw std::runtime_error("COMMON_CFG offset is past BAR region size");
  if (common.length == 0)
    throw std::runtime_error("COMMON_CFG length is zero");
  if (common.length < VIRTIO_PCI_COMMON_Q_USEDHI + sizeof(std::uint32_t)) {
    throw std::runtime_error("COMMON_CFG is shorter than queue address fields");
  }

  std::uint64_t available = info.size - common.offset;
  std::size_t common_length =
      static_cast<std::size_t>(std::min<std::uint64_t>(common.length, available));
  std::size_t inspect_length =
      std::min<std::size_t>(common_length, VIRTIO_PCI_COMMON_Q_USEDHI +
                                               sizeof(std::uint32_t));
#ifdef VIRTIO_PCI_COMMON_Q_RESET
  inspect_length = std::min<std::size_t>(
      common_length, VIRTIO_PCI_COMMON_Q_RESET + sizeof(std::uint16_t));
#endif

  if (!region.mmap_areas.empty()) {
    bool inside_sparse_area = false;
    for (const MmapArea &area : region.mmap_areas) {
      if (area.contains(common.offset, inspect_length)) {
        inside_sparse_area = true;
        break;
      }
    }
    if (!inside_sparse_area) {
      throw std::runtime_error(
          "COMMON_CFG range is not fully inside a VFIO sparse mmap area");
    }
  }

  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("cannot determine system page size");

  std::uint64_t page_mask = static_cast<std::uint64_t>(page_size - 1);
  std::uint64_t aligned_offset = common.offset & ~page_mask;
  std::size_t mapping_delta =
      static_cast<std::size_t>(common.offset - aligned_offset);
  std::size_t mapping_size = mapping_delta + inspect_length;
  off_t mmap_offset = static_cast<off_t>(info.offset + aligned_offset);

  MappedRegion mapping(vfio.device.get(), mapping_size, mmap_offset, yes);

  auto read8 = [&](std::size_t off) { return mapping.read8(mapping_delta + off); };
  auto read16 = [&](std::size_t off) {
    return mapping.read16(mapping_delta + off);
  };

  std::uint16_t num_queues = read16(VIRTIO_PCI_COMMON_NUMQ);
  std::uint16_t original_select = read16(VIRTIO_PCI_COMMON_Q_SELECT);
  std::uint8_t device_status = read8(VIRTIO_PCI_COMMON_STATUS);

  std::println("COMMON_CFG capability:");
  std::println("  bar: {}", common.bar);
  std::println("  offset: 0x{:x}", common.offset);
  std::println("  length: 0x{:x} ({})", common.length, common.length);
  std::println("device_status: 0x{:02x} ({})", device_status,
               status_names(device_status));
  std::println("num_queues: {}", num_queues);
  std::println("original queue_select: {}", original_select);

  if (requested_queue && *requested_queue >= num_queues) {
    throw std::runtime_error("requested queue " +
                             std::to_string(*requested_queue) +
                             " is outside num_queues=" +
                             std::to_string(num_queues));
  }

  if (!yes) {
    if (requested_queue) {
      std::println("Would write queue_select={} and read that queue",
                   *requested_queue);
    } else {
      std::println("Would write queue_select=0..{} and read each queue",
                   num_queues == 0 ? 0 : num_queues - 1);
    }
    std::println("Current selected queue view:");
    print_queue_view(mapping, mapping_delta);
    std::println("Pass --yes to write queue_select for enumeration.");
    return;
  }

  std::uint16_t first = requested_queue.value_or(0);
  std::uint16_t last =
      requested_queue.value_or(num_queues == 0 ? 0 : num_queues - 1);

  bool changed_selection = false;
  try {
    for (std::uint16_t queue = first; queue <= last; ++queue) {
      write_queue_select(mapping, mapping_delta, queue);
      changed_selection = true;
      std::uint16_t selected = read16(VIRTIO_PCI_COMMON_Q_SELECT);
      std::println("queue {}:", queue);
      std::println("    selected: {}", selected);
      print_queue_view(mapping, mapping_delta);
      if (queue == std::numeric_limits<std::uint16_t>::max())
        break;
    }
  } catch (...) {
    if (changed_selection) {
      write_queue_select(mapping, mapping_delta, original_select);
      std::println("Restored queue_select to {}", original_select);
    }
    throw;
  }

  if (changed_selection) {
    write_queue_select(mapping, mapping_delta, original_select);
    std::println("Restored queue_select to {}", original_select);
  }
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
    std::optional<std::uint16_t> queue;

    for (int i = 2; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "--show") {
        show = true;
      } else if (arg == "--queue" && i + 1 < argc) {
        queue = parse_u16(argv[++i], "--queue");
      } else if (arg == "--dry-run") {
        yes = false;
      } else if (arg == "--yes") {
        yes = true;
      } else {
        usage(argv[0]);
        return 1;
      }
    }

    if (!show) {
      usage(argv[0]);
      return 1;
    }

    show_queue_info(bdf, dev_dir, queue, yes);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
