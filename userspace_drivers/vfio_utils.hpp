#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>
#include <linux/virtio_pci.h>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

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

  void zero() { std::memset(data_, 0, size_); }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  std::uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
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

  void write8(std::size_t offset, std::uint8_t value) {
    check_range(offset, sizeof(std::uint8_t));
    *(base_ + offset) = value;
  }

  void write16(std::size_t offset, std::uint16_t value) {
    check_range(offset, sizeof(std::uint16_t));
    *reinterpret_cast<volatile std::uint16_t *>(base_ + offset) = value;
  }

  void write32(std::size_t offset, std::uint32_t value) {
    check_range(offset, sizeof(std::uint32_t));
    *reinterpret_cast<volatile std::uint32_t *>(base_ + offset) = value;
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

class DmaMapping {
public:
  DmaMapping(int container_fd, void *vaddr, std::uint64_t iova,
             std::size_t size, std::uint32_t flags)
      : container_fd_(container_fd), iova_(iova), size_(size) {
    vfio_iommu_type1_dma_map map{};
    map.argsz = sizeof(map);
    map.flags = flags;
    map.vaddr = reinterpret_cast<std::uintptr_t>(vaddr);
    map.iova = iova;
    map.size = size;

    if (::ioctl(container_fd_, VFIO_IOMMU_MAP_DMA, &map) == -1) {
      int error = errno;
      throw std::runtime_error("VFIO_IOMMU_MAP_DMA: " + errno_text(error));
    }
    active_ = true;
  }

  DmaMapping(const DmaMapping &) = delete;
  DmaMapping &operator=(const DmaMapping &) = delete;

  ~DmaMapping() { unmap_noexcept(); }

  std::uint64_t iova() const { return iova_; }
  std::size_t size() const { return size_; }

  std::uint64_t unmap() {
    if (!active_)
      return 0;

    vfio_iommu_type1_dma_unmap unmap{};
    unmap.argsz = sizeof(unmap);
    unmap.iova = iova_;
    unmap.size = size_;
    if (::ioctl(container_fd_, VFIO_IOMMU_UNMAP_DMA, &unmap) == -1) {
      int error = errno;
      throw std::runtime_error("VFIO_IOMMU_UNMAP_DMA: " + errno_text(error));
    }
    active_ = false;
    return unmap.size;
  }

private:
  static std::string errno_text(int error) {
    return std::strerror(error);
  }

  void unmap_noexcept() noexcept {
    try {
      unmap();
    } catch (...) {
    }
  }

  int container_fd_ = -1;
  std::uint64_t iova_ = 0;
  std::size_t size_ = 0;
  bool active_ = false;
};

struct DriverInfo {
  bool bound = false;
  std::string name = "<unbound>";
  std::filesystem::path path;
};

struct IommuGroupInfo {
  std::string id;
  std::filesystem::path path;
};

struct VfioContext {
  UniqueFd container;
  UniqueFd group;
  UniqueFd device;
};

inline std::string vfio_errno_text(int error) {
  return std::strerror(error);
}

inline UniqueFd vfio_open_fd(const std::filesystem::path &path, int flags) {
  int fd = ::open(path.c_str(), flags | O_CLOEXEC);
  if (fd == -1) {
    int error = errno;
    std::string message = "cannot open " + path.string() + ": " +
                          vfio_errno_text(error);
    if (error == EBUSY && path.string().starts_with("/dev/vfio/")) {
      message +=
          "; the VFIO group is already open in another process. Check with "
          "fuser or lsof and stop the process that owns this group";
    }
    throw std::runtime_error(message);
  }
  return UniqueFd(fd);
}

inline void vfio_ioctl_checked(int fd, unsigned long request, void *arg,
                               std::string_view operation) {
  if (::ioctl(fd, request, arg) == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             vfio_errno_text(error));
  }
}

inline int vfio_ioctl_value(int fd, unsigned long request,
                            std::string_view operation) {
  int result = ::ioctl(fd, request);
  if (result == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             vfio_errno_text(error));
  }
  return result;
}

inline int vfio_ioctl_arg_value(int fd, unsigned long request,
                                unsigned long arg,
                                std::string_view operation) {
  int result = ::ioctl(fd, request, arg);
  if (result == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             vfio_errno_text(error));
  }
  return result;
}

inline DriverInfo current_driver(const std::filesystem::path &dev_dir) {
  std::filesystem::path driver_link = dev_dir / "driver";
  if (!std::filesystem::exists(driver_link))
    return {};

  DriverInfo info;
  info.bound = true;

  std::filesystem::path target = std::filesystem::read_symlink(driver_link);
  info.name = target.filename().string();

  if (target.is_absolute())
    info.path = target;
  else
    info.path =
        std::filesystem::weakly_canonical(driver_link.parent_path() / target);

  return info;
}

inline void require_vfio_driver(const std::filesystem::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);
  if (driver.name != "vfio-pci") {
    throw std::runtime_error("device must be bound to vfio-pci; current driver=" +
                             driver.name);
  }
}

inline IommuGroupInfo
iommu_group_for_device(const std::filesystem::path &dev_dir) {
  std::filesystem::path group_link = dev_dir / "iommu_group";
  if (!std::filesystem::exists(group_link)) {
    throw std::runtime_error(dev_dir.string() +
                             " does not have an iommu_group symlink");
  }

  std::filesystem::path target = std::filesystem::read_symlink(group_link);
  std::filesystem::path group_path =
      target.is_absolute()
          ? target
          : std::filesystem::weakly_canonical(group_link.parent_path() /
                                              target);

  return {.id = group_path.filename().string(), .path = group_path};
}

inline std::vector<std::filesystem::path>
group_devices(const IommuGroupInfo &group) {
  std::vector<std::filesystem::path> devices;
  std::filesystem::path devices_dir = group.path / "devices";
  if (!std::filesystem::exists(devices_dir))
    return devices;

  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(devices_dir)) {
    std::filesystem::path target = std::filesystem::read_symlink(entry.path());
    std::filesystem::path dev_path =
        target.is_absolute()
            ? target
            : std::filesystem::weakly_canonical(entry.path().parent_path() /
                                                target);
    devices.push_back(dev_path);
  }

  std::sort(devices.begin(), devices.end());
  return devices;
}

inline void print_group_devices(const IommuGroupInfo &group) {
  std::println("IOMMU group devices:");
  for (const std::filesystem::path &dev_path : group_devices(group)) {
    DriverInfo driver = current_driver(dev_path);
    std::println("  {}  driver={}", dev_path.filename().string(), driver.name);
  }
}

inline VfioContext open_vfio_context(const std::string &bdf,
                                     const std::filesystem::path &dev_dir) {
  IommuGroupInfo group = iommu_group_for_device(dev_dir);
  std::println("IOMMU group: {} ({})", group.id, group.path.string());
  print_group_devices(group);

  VfioContext vfio;
  vfio.container = vfio_open_fd("/dev/vfio/vfio", O_RDWR);

  int api_version =
      vfio_ioctl_value(vfio.container.get(), VFIO_GET_API_VERSION,
                       "VFIO_GET_API_VERSION");
  if (api_version != VFIO_API_VERSION) {
    throw std::runtime_error("unsupported VFIO API version " +
                             std::to_string(api_version));
  }

  int type1 = vfio_ioctl_arg_value(
      vfio.container.get(), VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU,
      "VFIO_CHECK_EXTENSION(VFIO_TYPE1_IOMMU)");
  int type1v2 = vfio_ioctl_arg_value(
      vfio.container.get(), VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU,
      "VFIO_CHECK_EXTENSION(VFIO_TYPE1v2_IOMMU)");
  if (!type1 && !type1v2)
    throw std::runtime_error("VFIO Type1 IOMMU is not supported");

  vfio.group = vfio_open_fd(std::filesystem::path("/dev/vfio") / group.id,
                            O_RDWR);

  vfio_group_status group_status{};
  group_status.argsz = sizeof(group_status);
  vfio_ioctl_checked(vfio.group.get(), VFIO_GROUP_GET_STATUS, &group_status,
                     "VFIO_GROUP_GET_STATUS");
  if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    throw std::runtime_error(
        "IOMMU group is not viable; every device in the group must be bound "
        "to a VFIO-compatible driver or safely unbound");
  }

  int container_fd = vfio.container.get();
  vfio_ioctl_checked(vfio.group.get(), VFIO_GROUP_SET_CONTAINER,
                     &container_fd, "VFIO_GROUP_SET_CONTAINER");

  int iommu_type = type1v2 ? VFIO_TYPE1v2_IOMMU : VFIO_TYPE1_IOMMU;
  vfio_ioctl_arg_value(vfio.container.get(), VFIO_SET_IOMMU, iommu_type,
                       "VFIO_SET_IOMMU");
  std::println("container IOMMU type: {}",
               iommu_type == VFIO_TYPE1v2_IOMMU ? "VFIO_TYPE1v2_IOMMU"
                                                 : "VFIO_TYPE1_IOMMU");

  int raw_device_fd = ::ioctl(vfio.group.get(), VFIO_GROUP_GET_DEVICE_FD,
                              bdf.c_str());
  if (raw_device_fd == -1) {
    int error = errno;
    throw std::runtime_error("VFIO_GROUP_GET_DEVICE_FD: " +
                             vfio_errno_text(error));
  }
  vfio.device = UniqueFd(raw_device_fd);

  return vfio;
}

inline void print_iommu_info(int container_fd) {
  vfio_iommu_type1_info info{};
  info.argsz = sizeof(info);
  vfio_ioctl_checked(container_fd, VFIO_IOMMU_GET_INFO, &info,
                     "VFIO_IOMMU_GET_INFO");

  if (info.flags & VFIO_IOMMU_INFO_PGSIZES)
    std::println("IOMMU page-size bitmap: 0x{:x}", info.iova_pgsizes);
  else
    std::println("IOMMU page-size bitmap: <not reported>");
}

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

inline ConfigRegion get_config_region(int device_fd) {
  vfio_region_info region{};
  region.argsz = sizeof(region);
  region.index = VFIO_PCI_CONFIG_REGION_INDEX;
  vfio_ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region,
                     "VFIO_DEVICE_GET_REGION_INFO(CONFIG)");

  return {.offset = region.offset, .size = region.size};
}

inline void parse_region_caps(RegionInfo &region,
                              const std::vector<char> &buf) {
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

inline RegionInfo get_region_info(int device_fd, __u32 index) {
  vfio_region_info region{};
  region.argsz = sizeof(region);
  region.index = index;
  vfio_ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region,
                     "VFIO_DEVICE_GET_REGION_INFO");

  RegionInfo result;
  result.info = region;

  if ((region.flags & VFIO_REGION_INFO_FLAG_CAPS) &&
      region.argsz > sizeof(vfio_region_info)) {
    std::vector<char> buf(region.argsz);
    auto *full = reinterpret_cast<vfio_region_info *>(buf.data());
    full->argsz = static_cast<__u32>(buf.size());
    full->index = index;
    vfio_ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, full,
                       "VFIO_DEVICE_GET_REGION_INFO");
    result.info = *full;
    parse_region_caps(result, buf);
  }

  return result;
}

inline void pread_exact(int fd, void *data, std::size_t size,
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
      throw std::runtime_error(std::string(what) + ": " +
                               vfio_errno_text(error));
    }
    if (got == 0)
      throw std::runtime_error(std::string(what) + ": short read");
    done += static_cast<std::size_t>(got);
  }
}

inline void pwrite_exact(int fd, const void *data, std::size_t size,
                         std::uint64_t offset, std::string_view what) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    throw std::runtime_error(std::string(what) + ": offset too large");

  const auto *in = static_cast<const std::uint8_t *>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t wrote = ::pwrite(fd, in + done, size - done,
                             static_cast<off_t>(offset + done));
    if (wrote == -1 && errno == EINTR)
      continue;
    if (wrote == -1) {
      int error = errno;
      throw std::runtime_error(std::string(what) + ": " +
                               vfio_errno_text(error));
    }
    if (wrote == 0)
      throw std::runtime_error(std::string(what) + ": short write");
    done += static_cast<std::size_t>(wrote);
  }
}

inline void read_config_bytes(int device_fd, const ConfigRegion &config,
                              std::uint16_t offset, void *data,
                              std::size_t size) {
  if (static_cast<std::uint64_t>(offset) + size > config.size)
    throw std::runtime_error("PCI config read past VFIO CONFIG region");
  pread_exact(device_fd, data, size, config.offset + offset, "pread CONFIG");
}

inline void write_config_bytes(int device_fd, const ConfigRegion &config,
                               std::uint16_t offset, const void *data,
                               std::size_t size) {
  if (static_cast<std::uint64_t>(offset) + size > config.size)
    throw std::runtime_error("PCI config write past VFIO CONFIG region");
  pwrite_exact(device_fd, data, size, config.offset + offset, "pwrite CONFIG");
}

inline std::uint8_t read_config_u8(int device_fd, const ConfigRegion &config,
                                   std::uint16_t offset) {
  std::uint8_t value = 0;
  read_config_bytes(device_fd, config, offset, &value, sizeof(value));
  return value;
}

inline std::uint16_t read_config_le16(int device_fd,
                                      const ConfigRegion &config,
                                      std::uint16_t offset) {
  std::uint8_t bytes[2]{};
  read_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

inline void write_config_le16(int device_fd, const ConfigRegion &config,
                              std::uint16_t offset, std::uint16_t value) {
  std::uint8_t bytes[2]{
      static_cast<std::uint8_t>(value & 0xffu),
      static_cast<std::uint8_t>(value >> 8),
  };
  write_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
}

inline std::uint32_t read_config_le32(int device_fd,
                                      const ConfigRegion &config,
                                      std::uint16_t offset) {
  std::uint8_t bytes[4]{};
  read_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::uint64_t read_config_le64_from_halves(int device_fd,
                                                  const ConfigRegion &config,
                                                  std::uint16_t lo_offset,
                                                  std::uint16_t hi_offset) {
  std::uint64_t lo = read_config_le32(device_fd, config, lo_offset);
  std::uint64_t hi = read_config_le32(device_fd, config, hi_offset);
  return lo | (hi << 32);
}

struct VirtioCap {
  std::uint8_t cfg_type = 0;
  std::uint8_t bar = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::uint32_t notify_off_multiplier = 0;
};

inline std::vector<VirtioCap> read_virtio_caps(int device_fd,
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

    std::uint8_t cap_id =
        read_config_u8(device_fd, config, cap + PCI_CAP_LIST_ID);
    std::uint8_t next =
        read_config_u8(device_fd, config, cap + PCI_CAP_LIST_NEXT);
    if (cap_id == PCI_CAP_ID_VNDR) {
      std::uint8_t cap_len = read_config_u8(device_fd, config, cap + 2);
      if (cap_len >= sizeof(virtio_pci_cap) &&
          static_cast<std::uint64_t>(cap) + cap_len <= config.size) {
        VirtioCap virtio;
        virtio.cfg_type =
            read_config_u8(device_fd, config, cap + VIRTIO_PCI_CAP_CFG_TYPE);
        virtio.bar =
            read_config_u8(device_fd, config, cap + VIRTIO_PCI_CAP_BAR);
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
        if (virtio.cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
          if (cap_len < sizeof(virtio_pci_notify_cap))
            throw std::runtime_error("NOTIFY_CFG capability is truncated");
          virtio.notify_off_multiplier = read_config_le32(
              device_fd, config, cap + VIRTIO_PCI_NOTIFY_CAP_MULT);
        }
        caps.push_back(virtio);
      }
    }
    cap = next;
  }
  return caps;
}

inline VirtioCap find_common_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
      return cap;
  }
  throw std::runtime_error("virtio COMMON_CFG capability was not found");
}

inline VirtioCap find_notify_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG)
      return cap;
  }
  throw std::runtime_error("virtio NOTIFY_CFG capability was not found");
}

inline VirtioCap find_device_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG)
      return cap;
  }
  throw std::runtime_error("virtio DEVICE_CFG capability was not found");
}

inline __u32 vfio_bar_region_index(std::uint8_t bar) {
  if (bar > 5)
    throw std::runtime_error("virtio capability references invalid BAR " +
                             std::to_string(bar));
  return VFIO_PCI_BAR0_REGION_INDEX + bar;
}

inline long vfio_system_page_size() {
  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("cannot determine system page size");
  return page_size;
}

inline void require_mmap_area_contains(const RegionInfo &region,
                                       std::uint64_t range_offset,
                                       std::uint64_t range_size,
                                       std::string_view what) {
  if (region.mmap_areas.empty())
    return;

  for (const MmapArea &area : region.mmap_areas) {
    if (area.contains(range_offset, range_size))
      return;
  }

  throw std::runtime_error(std::string(what) +
                           " is not fully inside a VFIO sparse mmap area");
}

inline MappedRegion map_common_cfg(int device_fd, const VirtioCap &common,
                                   std::size_t &mapping_delta,
                                   bool writable) {
  __u32 region_index = vfio_bar_region_index(common.bar);
  RegionInfo region = get_region_info(device_fd, region_index);
  const vfio_region_info &info = region.info;

  if (info.size == 0)
    throw std::runtime_error("COMMON_CFG BAR region is not present");
  if (!(info.flags & VFIO_REGION_INFO_FLAG_MMAP))
    throw std::runtime_error("COMMON_CFG BAR region does not support mmap");
  if (common.offset >= info.size)
    throw std::runtime_error("COMMON_CFG offset is past BAR region size");
  if (common.length < VIRTIO_PCI_COMMON_Q_USEDHI + sizeof(std::uint32_t))
    throw std::runtime_error("COMMON_CFG is shorter than queue address fields");

  std::uint64_t available = info.size - common.offset;
  std::size_t common_length = static_cast<std::size_t>(
      std::min<std::uint64_t>(common.length, available));
  std::size_t required_length =
      VIRTIO_PCI_COMMON_Q_USEDHI + sizeof(std::uint32_t);
#ifdef VIRTIO_PCI_COMMON_Q_NDATA
  required_length =
      std::max<std::size_t>(required_length,
                            VIRTIO_PCI_COMMON_Q_NDATA + sizeof(std::uint16_t));
#endif
  std::size_t inspect_length =
      std::min<std::size_t>(common_length, required_length);

  require_mmap_area_contains(region, common.offset, inspect_length,
                             "COMMON_CFG range");

  long page_size = vfio_system_page_size();
  std::uint64_t page_mask = static_cast<std::uint64_t>(page_size - 1);
  std::uint64_t aligned_offset = common.offset & ~page_mask;
  mapping_delta = static_cast<std::size_t>(common.offset - aligned_offset);
  std::size_t mapping_size = mapping_delta + inspect_length;
  off_t mmap_offset = static_cast<off_t>(info.offset + aligned_offset);

  std::println("COMMON_CFG capability:");
  std::println("  bar: {}", common.bar);
  std::println("  offset: 0x{:x}", common.offset);
  std::println("  length: 0x{:x} ({})", common.length, common.length);
  std::println("mmap file offset: 0x{:x}", mmap_offset);

  return MappedRegion(device_fd, mapping_size, mmap_offset, writable);
}

inline MappedRegion map_device_cfg(int device_fd, const VirtioCap &device_cfg,
                                   std::size_t &mapping_delta,
                                   std::size_t required_length = 0) {
  __u32 region_index = vfio_bar_region_index(device_cfg.bar);
  RegionInfo region = get_region_info(device_fd, region_index);
  const vfio_region_info &info = region.info;

  if (info.size == 0)
    throw std::runtime_error("DEVICE_CFG BAR region is not present");
  if (!(info.flags & VFIO_REGION_INFO_FLAG_MMAP))
    throw std::runtime_error("DEVICE_CFG BAR region does not support mmap");
  if (device_cfg.offset >= info.size)
    throw std::runtime_error("DEVICE_CFG offset is past BAR region size");
  if (device_cfg.length < required_length)
    throw std::runtime_error("DEVICE_CFG is shorter than required config");

  std::uint64_t available = info.size - device_cfg.offset;
  std::size_t inspect_length = static_cast<std::size_t>(
      std::min<std::uint64_t>(device_cfg.length, available));

  require_mmap_area_contains(region, device_cfg.offset, inspect_length,
                             "DEVICE_CFG range");

  long page_size = vfio_system_page_size();
  std::uint64_t page_mask = static_cast<std::uint64_t>(page_size - 1);
  std::uint64_t aligned_offset = device_cfg.offset & ~page_mask;
  mapping_delta = static_cast<std::size_t>(device_cfg.offset - aligned_offset);
  std::size_t mapping_size = mapping_delta + inspect_length;
  off_t mmap_offset = static_cast<off_t>(info.offset + aligned_offset);

  std::println("DEVICE_CFG capability:");
  std::println("  bar: {}", device_cfg.bar);
  std::println("  offset: 0x{:x}", device_cfg.offset);
  std::println("  length: 0x{:x} ({})", device_cfg.length, device_cfg.length);
  std::println("mmap file offset: 0x{:x}", mmap_offset);

  return MappedRegion(device_fd, mapping_size, mmap_offset, false);
}

inline MappedRegion map_notify_window(int device_fd, const VirtioCap &notify,
                                      const RegionInfo &region,
                                      std::uint16_t queue_notify_off,
                                      std::size_t &mapping_delta,
                                      std::uint64_t &bar_offset,
                                      off_t &mmap_offset) {
  const vfio_region_info &info = region.info;
  if (info.size == 0)
    throw std::runtime_error("NOTIFY_CFG BAR region is not present");
  if (!(info.flags & VFIO_REGION_INFO_FLAG_MMAP))
    throw std::runtime_error("NOTIFY_CFG BAR region does not support mmap");
  if (!(info.flags & VFIO_REGION_INFO_FLAG_WRITE))
    throw std::runtime_error("NOTIFY_CFG BAR region is not writable");

  bar_offset =
      notify.offset + static_cast<std::uint64_t>(queue_notify_off) *
                          notify.notify_off_multiplier;
  if (bar_offset + sizeof(std::uint16_t) > info.size)
    throw std::runtime_error("computed notify offset is past BAR region size");
  if (bar_offset < notify.offset ||
      bar_offset + sizeof(std::uint16_t) > notify.offset + notify.length) {
    throw std::runtime_error("computed notify offset is outside NOTIFY_CFG");
  }

  require_mmap_area_contains(region, bar_offset, sizeof(std::uint16_t),
                             "notify range");

  long page_size = vfio_system_page_size();
  std::uint64_t page_mask = static_cast<std::uint64_t>(page_size - 1);
  std::uint64_t aligned_offset = bar_offset & ~page_mask;
  mapping_delta = static_cast<std::size_t>(bar_offset - aligned_offset);
  mmap_offset = static_cast<off_t>(info.offset + aligned_offset);
  std::size_t mapping_size = mapping_delta + sizeof(std::uint16_t);
  return MappedRegion(device_fd, mapping_size, mmap_offset, true);
}
