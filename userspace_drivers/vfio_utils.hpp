#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <linux/vfio.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
