// Exercise 19: Write one virtio queue notification through VFIO
//
// Goal: write one 16-bit notify value to a queue's modern virtio NOTIFY_CFG
// MMIO location, while still stopping before DRIVER_OK.
//
// Background
// ----------
// Exercise 18 computed the notify MMIO location but deliberately did not write
// it.  This sample performs one notify write after preparing and enabling a
// queue:
//
//   reset -> ACKNOWLEDGE -> DRIVER -> FEATURES_OK
//   map vring memory
//   write queue_select
//   write queue_size / queue_desc / queue_avail / queue_used
//   write queue_enable = 1
//   write queue index to NOTIFY_CFG + queue_notify_off * multiplier
//
// It still does not set DRIVER_OK.  Without DRIVER_OK, the device should not
// start normal operation.  Before unmapping the vring memory, this sample
// resets the device so it does not keep an enabled queue pointing at unmapped
// userspace memory.
//
// New concepts
// ------------
// - Mapping the NOTIFY_CFG MMIO window
// - Writing the 16-bit modern virtio queue notification value
// - Keeping DMA memory mapped while an enabled queue references it
// - Reset cleanup before VFIO_IOMMU_UNMAP_DMA

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
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
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static_assert(std::endian::native == std::endian::little,
              "this tutorial sample expects a little-endian host");

constexpr std::size_t kFeatureWords = 3;
constexpr std::size_t kVringDescSize = 16;
constexpr std::size_t kVringUsedElemSize = 8;
constexpr std::size_t kVringUsedAlignSize = 4;

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
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::uint32_t notify_off_multiplier = 0;
};

struct FeatureWords {
  std::uint32_t original_select = 0;
  std::vector<std::uint32_t> words;
};

struct QueueView {
  std::uint16_t selected = 0;
  std::uint16_t size = 0;
  std::uint16_t msix_vector = 0;
  std::uint16_t enable = 0;
  std::uint16_t notify_off = 0;
  std::optional<std::uint16_t> notify_data;
  std::uint64_t desc = 0;
  std::uint64_t avail = 0;
  std::uint64_t used = 0;
};

struct VringLayout {
  std::uint16_t queue_size = 0;
  std::size_t align = 0;
  std::size_t desc_offset = 0;
  std::size_t desc_size = 0;
  std::size_t avail_offset = 0;
  std::size_t avail_size = 0;
  std::size_t used_offset = 0;
  std::size_t used_size = 0;
  std::size_t total_size = 0;
};

struct Options {
  std::uint16_t queue = 0;
  std::optional<std::uint16_t> queue_size;
  std::uint64_t iova = 0x100000000ull;
  std::size_t align = 4096;
  bool yes = false;
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
               "  {} <BDF> [--queue <n>] [--queue-size <n>] [--iova <addr>]\n"
               "       [--align <bytes>] [--dry-run|--yes]\n\n"
               "Defaults:\n"
               "  queue = 0\n"
               "  queue-size = target queue's device-reported size\n"
               "  iova = 0x100000000\n"
               "  align = 4096\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.6 --queue 0\n"
               "  {} c1:00.6 --queue 0 --yes\n"
               "  {} c1:00.6 --queue 0 --queue-size 128 --iova 0x200000000 --yes",
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

static bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static std::size_t align_up(std::size_t value, std::size_t align) {
  if (!is_power_of_two(align))
    throw std::runtime_error("alignment must be a power of two");
  if (value > std::numeric_limits<std::size_t>::max() - (align - 1))
    throw std::runtime_error("size overflow while aligning value");
  return (value + align - 1) & ~(align - 1);
}

static long system_page_size() {
  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("cannot determine system page size");
  return page_size;
}

static std::size_t round_up_to_page(std::size_t size) {
  return align_up(size, static_cast<std::size_t>(system_page_size()));
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

static std::string dma_flag_names(std::uint32_t flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_DMA_MAP_FLAG_READ)
    names.push_back("READ");
  if (flags & VFIO_DMA_MAP_FLAG_WRITE)
    names.push_back("WRITE");
  return join_flags(names);
}

static void print_status(std::string_view label, std::uint8_t status) {
  std::println("{}: 0x{:02x} ({})", label, status, status_names(status));
}

static bool feature_is_set(const std::vector<std::uint32_t> &words,
                           unsigned bit) {
  std::size_t word = bit / 32;
  unsigned shift = bit % 32;
  return word < words.size() && (words[word] & (1u << shift));
}

static void set_feature(std::vector<std::uint32_t> &words, unsigned bit) {
  std::size_t word = bit / 32;
  unsigned shift = bit % 32;
  if (word >= words.size())
    throw std::runtime_error("feature bit is outside the configured word range");
  words[word] |= 1u << shift;
}

static void print_feature_words(std::string_view label,
                                const std::vector<std::uint32_t> &words) {
  std::println("{}:", label);
  for (std::size_t i = 0; i < words.size(); ++i) {
    std::println("  word {} (bits {}..{}): 0x{:08x}", i, i * 32,
                 i * 32 + 31, words[i]);
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

class QueueSelectionGuard {
public:
  QueueSelectionGuard(MappedRegion &mapping, std::size_t mapping_delta,
                      std::uint16_t original_select)
      : mapping_(&mapping), mapping_delta_(mapping_delta),
        original_select_(original_select) {}

  QueueSelectionGuard(const QueueSelectionGuard &) = delete;
  QueueSelectionGuard &operator=(const QueueSelectionGuard &) = delete;

  ~QueueSelectionGuard() { restore_noexcept(); }

  void select(std::uint16_t queue) {
    mapping_->write16(mapping_delta_ + VIRTIO_PCI_COMMON_Q_SELECT, queue);
    changed_ = true;
  }

  void restore() {
    if (!changed_)
      return;
    mapping_->write16(mapping_delta_ + VIRTIO_PCI_COMMON_Q_SELECT,
                      original_select_);
    changed_ = false;
  }

private:
  void restore_noexcept() noexcept {
    try {
      restore();
    } catch (...) {
    }
  }

  MappedRegion *mapping_ = nullptr;
  std::size_t mapping_delta_ = 0;
  std::uint16_t original_select_ = 0;
  bool changed_ = false;
};

class DeviceResetGuard {
public:
  DeviceResetGuard(MappedRegion &mapping, std::size_t mapping_delta)
      : mapping_(&mapping), mapping_delta_(mapping_delta) {}

  DeviceResetGuard(const DeviceResetGuard &) = delete;
  DeviceResetGuard &operator=(const DeviceResetGuard &) = delete;

  ~DeviceResetGuard() { reset_noexcept(); }

  void disarm() { active_ = false; }

private:
  void reset_noexcept() noexcept {
    if (!active_)
      return;
    try {
      mapping_->write8(mapping_delta_ + VIRTIO_PCI_COMMON_STATUS, 0);
    } catch (...) {
    }
  }

  MappedRegion *mapping_ = nullptr;
  std::size_t mapping_delta_ = 0;
  bool active_ = true;
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

    ioctl_checked(container_fd_, VFIO_IOMMU_MAP_DMA, &map,
                  "VFIO_IOMMU_MAP_DMA");
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
    ioctl_checked(container_fd_, VFIO_IOMMU_UNMAP_DMA, &unmap,
                  "VFIO_IOMMU_UNMAP_DMA");
    active_ = false;
    return unmap.size;
  }

private:
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

static VirtioCap find_common_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
      return cap;
  }
  throw std::runtime_error("virtio COMMON_CFG capability was not found");
}

static VirtioCap find_notify_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG)
      return cap;
  }
  throw std::runtime_error("virtio NOTIFY_CFG capability was not found");
}

static __u32 vfio_bar_region_index(std::uint8_t bar) {
  if (bar > 5)
    throw std::runtime_error("virtio capability references invalid BAR " +
                             std::to_string(bar));
  return VFIO_PCI_BAR0_REGION_INDEX + bar;
}

static MappedRegion map_common_cfg(int device_fd, const VirtioCap &common,
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
  std::size_t common_length =
      static_cast<std::size_t>(std::min<std::uint64_t>(common.length, available));
  std::size_t required_length = VIRTIO_PCI_COMMON_Q_USEDHI + sizeof(std::uint32_t);
#ifdef VIRTIO_PCI_COMMON_Q_NDATA
  required_length =
      std::max<std::size_t>(required_length,
                            VIRTIO_PCI_COMMON_Q_NDATA + sizeof(std::uint16_t));
#endif
  std::size_t inspect_length =
      std::min<std::size_t>(common_length, required_length);

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

  long page_size = system_page_size();
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

static MappedRegion map_notify_window(int device_fd, const VirtioCap &notify,
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

  if (!region.mmap_areas.empty()) {
    bool inside_sparse_area = false;
    for (const MmapArea &area : region.mmap_areas) {
      if (area.contains(bar_offset, sizeof(std::uint16_t))) {
        inside_sparse_area = true;
        break;
      }
    }
    if (!inside_sparse_area) {
      throw std::runtime_error(
          "notify range is not fully inside a VFIO sparse mmap area");
    }
  }

  long page_size = system_page_size();
  std::uint64_t page_mask = static_cast<std::uint64_t>(page_size - 1);
  std::uint64_t aligned_offset = bar_offset & ~page_mask;
  mapping_delta = static_cast<std::size_t>(bar_offset - aligned_offset);
  mmap_offset = static_cast<off_t>(info.offset + aligned_offset);
  std::size_t mapping_size = mapping_delta + sizeof(std::uint16_t);
  return MappedRegion(device_fd, mapping_size, mmap_offset, true);
}

static std::uint8_t read_status(MappedRegion &mapping,
                                std::size_t mapping_delta) {
  return mapping.read8(mapping_delta + VIRTIO_PCI_COMMON_STATUS);
}

static void write_status(MappedRegion &mapping, std::size_t mapping_delta,
                         std::uint8_t status) {
  mapping.write8(mapping_delta + VIRTIO_PCI_COMMON_STATUS, status);
}

static void wait_for_status(MappedRegion &mapping, std::size_t mapping_delta,
                            std::uint8_t expected,
                            std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (read_status(mapping, mapping_delta) == expected)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::uint8_t actual = read_status(mapping, mapping_delta);
  throw std::runtime_error("timed out waiting for device_status " +
                           status_names(expected) + ", actual=" +
                           status_names(actual));
}

static void reset_device(MappedRegion &mapping, std::size_t mapping_delta,
                         std::string_view label) {
  write_status(mapping, mapping_delta, 0);
  wait_for_status(mapping, mapping_delta, 0, std::chrono::milliseconds(1000));
  print_status(label, read_status(mapping, mapping_delta));
}

static std::uint64_t read_common_u64(MappedRegion &mapping,
                                     std::size_t mapping_delta,
                                     std::size_t lo_offset,
                                     std::size_t hi_offset) {
  std::uint64_t lo = mapping.read32(mapping_delta + lo_offset);
  std::uint64_t hi = mapping.read32(mapping_delta + hi_offset);
  return lo | (hi << 32);
}

static void write_common_u64(MappedRegion &mapping, std::size_t mapping_delta,
                             std::size_t lo_offset, std::size_t hi_offset,
                             std::uint64_t value) {
  mapping.write32(mapping_delta + lo_offset,
                  static_cast<std::uint32_t>(value & 0xffffffffu));
  mapping.write32(mapping_delta + hi_offset,
                  static_cast<std::uint32_t>(value >> 32));
}

static QueueView read_queue_view(MappedRegion &mapping,
                                 std::size_t mapping_delta) {
  QueueView view;
  view.selected = mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_SELECT);
  view.size = mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_SIZE);
  view.msix_vector = mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_MSIX);
  view.enable = mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_ENABLE);
  view.notify_off = mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_NOFF);
#ifdef VIRTIO_PCI_COMMON_Q_NDATA
  view.notify_data =
      mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_NDATA);
#endif
  view.desc = read_common_u64(mapping, mapping_delta,
                              VIRTIO_PCI_COMMON_Q_DESCLO,
                              VIRTIO_PCI_COMMON_Q_DESCHI);
  view.avail = read_common_u64(mapping, mapping_delta,
                               VIRTIO_PCI_COMMON_Q_AVAILLO,
                               VIRTIO_PCI_COMMON_Q_AVAILHI);
  view.used = read_common_u64(mapping, mapping_delta,
                              VIRTIO_PCI_COMMON_Q_USEDLO,
                              VIRTIO_PCI_COMMON_Q_USEDHI);
  return view;
}

static void print_queue_view(std::string_view label, const QueueView &view) {
  std::println("{}:", label);
  std::println("  selected: {}", view.selected);
  std::println("  queue_size: {}", view.size);
  std::println("  queue_msix_vector: 0x{:04x} ({})", view.msix_vector,
               view.msix_vector);
  std::println("  queue_enable: {}", view.enable);
  std::println("  queue_notify_off: {}", view.notify_off);
  if (view.notify_data)
    std::println("  queue_notify_data: {}", *view.notify_data);
  std::println("  queue_desc: 0x{:016x}", view.desc);
  std::println("  queue_avail: 0x{:016x}", view.avail);
  std::println("  queue_used: 0x{:016x}", view.used);
}

static FeatureWords read_feature_words(MappedRegion &mapping,
                                       std::size_t mapping_delta,
                                       std::size_t select_offset,
                                       std::size_t value_offset,
                                       std::size_t word_count) {
  FeatureWords result;
  result.original_select = mapping.read32(mapping_delta + select_offset);
  result.words.reserve(word_count);

  for (std::size_t word = 0; word < word_count; ++word) {
    mapping.write32(mapping_delta + select_offset,
                    static_cast<std::uint32_t>(word));
    result.words.push_back(mapping.read32(mapping_delta + value_offset));
  }

  mapping.write32(mapping_delta + select_offset, result.original_select);
  return result;
}

static std::vector<std::uint32_t>
minimal_guest_features(const std::vector<std::uint32_t> &device_features) {
  std::vector<std::uint32_t> guest(kFeatureWords, 0);

  if (!feature_is_set(device_features, VIRTIO_F_VERSION_1)) {
    throw std::runtime_error(
        "device does not offer VIRTIO_F_VERSION_1; this sample only supports "
        "modern virtio");
  }

  set_feature(guest, VIRTIO_F_VERSION_1);
  if (feature_is_set(device_features, VIRTIO_F_ACCESS_PLATFORM))
    set_feature(guest, VIRTIO_F_ACCESS_PLATFORM);

  return guest;
}

static void write_guest_feature_words(MappedRegion &mapping,
                                      std::size_t mapping_delta,
                                      const std::vector<std::uint32_t> &words) {
  std::uint32_t original_select =
      mapping.read32(mapping_delta + VIRTIO_PCI_COMMON_GFSELECT);

  for (std::size_t word = 0; word < words.size(); ++word) {
    mapping.write32(mapping_delta + VIRTIO_PCI_COMMON_GFSELECT,
                    static_cast<std::uint32_t>(word));
    mapping.write32(mapping_delta + VIRTIO_PCI_COMMON_GF, words[word]);
    std::println("wrote guest_feature word {}: 0x{:08x}", word, words[word]);
  }

  mapping.write32(mapping_delta + VIRTIO_PCI_COMMON_GFSELECT, original_select);
  std::println("Restored guest_feature_select to {}", original_select);
}

static void negotiate_minimal_features(MappedRegion &mapping,
                                       std::size_t mapping_delta) {
  reset_device(mapping, mapping_delta, "after reset");

  write_status(mapping, mapping_delta, VIRTIO_CONFIG_S_ACKNOWLEDGE);
  print_status("after ACKNOWLEDGE", read_status(mapping, mapping_delta));

  write_status(mapping, mapping_delta,
               VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
  print_status("after DRIVER", read_status(mapping, mapping_delta));

  FeatureWords device_features =
      read_feature_words(mapping, mapping_delta, VIRTIO_PCI_COMMON_DFSELECT,
                         VIRTIO_PCI_COMMON_DF, kFeatureWords);
  print_feature_words("Device feature words", device_features.words);

  std::vector<std::uint32_t> guest_features =
      minimal_guest_features(device_features.words);
  print_feature_words("Minimal guest feature words", guest_features);
  write_guest_feature_words(mapping, mapping_delta, guest_features);

  constexpr std::uint8_t features_ok_status =
      VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
      VIRTIO_CONFIG_S_FEATURES_OK;
  write_status(mapping, mapping_delta, features_ok_status);

  std::uint8_t after_features_ok = read_status(mapping, mapping_delta);
  print_status("after FEATURES_OK", after_features_ok);
  if (!(after_features_ok & VIRTIO_CONFIG_S_FEATURES_OK)) {
    throw std::runtime_error("device rejected FEATURES_OK");
  }
}

static VringLayout compute_vring_layout(std::uint16_t queue_size,
                                        std::size_t align) {
  if (queue_size == 0)
    throw std::runtime_error("queue_size must be greater than 0");
  if (!is_power_of_two(queue_size))
    throw std::runtime_error("this sample expects a power-of-two queue size");

  VringLayout layout;
  layout.queue_size = queue_size;
  layout.align = align;
  layout.desc_offset = 0;
  layout.desc_size = kVringDescSize * queue_size;
  layout.avail_offset = layout.desc_offset + layout.desc_size;
  layout.avail_size = sizeof(std::uint16_t) * (3u + queue_size);
  layout.used_offset =
      align_up(layout.avail_offset + layout.avail_size, align);
  layout.used_size =
      sizeof(std::uint16_t) * 3u + kVringUsedElemSize * queue_size;
  layout.total_size = layout.used_offset + layout.used_size;
  return layout;
}

static void print_vring_layout(const VringLayout &layout,
                               std::uint64_t base_iova,
                               std::size_t mapped_size) {
  std::println("Split vring layout:");
  std::println("  queue_size: {}", layout.queue_size);
  std::println("  used-ring alignment: {}", layout.align);
  std::println("  queue_desc IOVA: 0x{:x}", base_iova + layout.desc_offset);
  std::println("  queue_avail IOVA: 0x{:x}", base_iova + layout.avail_offset);
  std::println("  queue_used IOVA: 0x{:x}", base_iova + layout.used_offset);
  std::println("  descriptor bytes: {}", layout.desc_size);
  std::println("  available bytes: {}", layout.avail_size);
  std::println("  used bytes: {}", layout.used_size);
  std::println("  vring bytes: {}", layout.total_size);
  std::println("  mapped bytes: {}", mapped_size);
}

static std::uint16_t choose_queue_size(std::uint16_t device_queue_size,
                                       std::optional<std::uint16_t> requested) {
  if (device_queue_size == 0)
    throw std::runtime_error("selected queue is unavailable");

  std::uint16_t chosen = requested.value_or(device_queue_size);
  if (chosen == 0 || chosen > device_queue_size) {
    throw std::runtime_error("requested queue-size must be in range 1.." +
                             std::to_string(device_queue_size));
  }
  if (!is_power_of_two(chosen))
    throw std::runtime_error("requested queue-size must be a power of two");
  return chosen;
}

static void dry_run(const std::string &bdf, const fs::path &dev_dir,
                    const Options &options) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);
  std::println("queue: {}", options.queue);
  std::println("base IOVA: 0x{:x}", options.iova);
  std::println("vring alignment: {}", options.align);

  if (options.queue_size) {
    VringLayout layout =
        compute_vring_layout(*options.queue_size, options.align);
    print_vring_layout(layout, options.iova, round_up_to_page(layout.total_size));
  } else {
    std::println("queue_size: target queue's device-reported size");
  }

  std::println("Would open VFIO container/group/device");
  std::println("Would mmap virtio COMMON_CFG writable");
  std::println("Would reset device and negotiate minimal FEATURES_OK");
  std::println("Would write queue_select={} and read queue_size", options.queue);
  std::println("Would allocate and VFIO-map zeroed split vring memory");
  std::println("Would write queue_size, queue_desc, queue_avail, queue_used");
  std::println("Would write queue_enable=1 and read the queue back");
  std::println("Would compute NOTIFY_CFG offset and write one notify value");
  std::println("Would restore queue_select, reset the device, then unmap DMA");
  std::println("Would not write DRIVER_OK.");
  std::println("Pass --yes to perform these device and DMA writes.");
}

static void program_queue_addresses(MappedRegion &mapping,
                                    std::size_t mapping_delta,
                                    const VringLayout &layout,
                                    std::uint64_t base_iova) {
  mapping.write16(mapping_delta + VIRTIO_PCI_COMMON_Q_SIZE,
                  layout.queue_size);
  write_common_u64(mapping, mapping_delta, VIRTIO_PCI_COMMON_Q_DESCLO,
                   VIRTIO_PCI_COMMON_Q_DESCHI, base_iova + layout.desc_offset);
  write_common_u64(mapping, mapping_delta, VIRTIO_PCI_COMMON_Q_AVAILLO,
                   VIRTIO_PCI_COMMON_Q_AVAILHI,
                   base_iova + layout.avail_offset);
  write_common_u64(mapping, mapping_delta, VIRTIO_PCI_COMMON_Q_USEDLO,
                   VIRTIO_PCI_COMMON_Q_USEDHI, base_iova + layout.used_offset);
}

static void enable_queue(MappedRegion &mapping, std::size_t mapping_delta) {
  mapping.write16(mapping_delta + VIRTIO_PCI_COMMON_Q_ENABLE, 1);
}

static void write_notify(MappedRegion &mapping, std::size_t mapping_delta,
                         std::uint16_t value) {
  mapping.write16(mapping_delta, value);
}

static void run_notify_write(const std::string &bdf, const fs::path &dev_dir,
                             const Options &options) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);
  std::println("queue: {}", options.queue);
  std::println("base IOVA: 0x{:x}", options.iova);
  std::println("vring alignment: {}", options.align);

  std::size_t page_size = static_cast<std::size_t>(system_page_size());
  if (options.iova % page_size != 0) {
    throw std::runtime_error("base IOVA must be aligned to the system page "
                             "size " +
                             std::to_string(page_size));
  }

  VfioContext vfio = open_vfio_context(bdf, dev_dir);
  print_iommu_info(vfio.container.get());

  ConfigRegion config = get_config_region(vfio.device.get());
  std::vector<VirtioCap> caps = read_virtio_caps(vfio.device.get(), config);
  VirtioCap common = find_common_cfg(caps);
  VirtioCap notify = find_notify_cfg(caps);

  RegionInfo notify_region =
      get_region_info(vfio.device.get(), vfio_bar_region_index(notify.bar));
  std::println("NOTIFY_CFG capability:");
  std::println("  bar: {}", notify.bar);
  std::println("  offset: 0x{:x}", notify.offset);
  std::println("  length: 0x{:x} ({})", notify.length, notify.length);
  std::println("  notify_off_multiplier: {}",
               notify.notify_off_multiplier);

  std::size_t mapping_delta = 0;
  MappedRegion common_mapping =
      map_common_cfg(vfio.device.get(), common, mapping_delta, true);

  print_status("initial device_status",
               read_status(common_mapping, mapping_delta));
  negotiate_minimal_features(common_mapping, mapping_delta);

  std::uint16_t num_queues =
      common_mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_NUMQ);
  std::uint16_t original_select =
      common_mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_SELECT);
  std::println("num_queues: {}", num_queues);
  std::println("original queue_select: {}", original_select);

  if (options.queue >= num_queues)
    throw std::runtime_error("queue index is outside num_queues");

  QueueSelectionGuard selection(common_mapping, mapping_delta, original_select);
  selection.select(options.queue);
  QueueView before = read_queue_view(common_mapping, mapping_delta);
  print_queue_view("Selected queue before programming", before);

  if (before.enable != 0) {
    throw std::runtime_error(
        "selected queue is already enabled; reset the device before using "
        "this tutorial sample");
  }

  std::uint16_t chosen_queue_size =
      choose_queue_size(before.size, options.queue_size);
  if (chosen_queue_size != before.size) {
    std::println("using reduced queue_size: {} (device max {})",
                 chosen_queue_size, before.size);
  }

  VringLayout layout = compute_vring_layout(chosen_queue_size, options.align);
  std::size_t mapped_size = round_up_to_page(layout.total_size);
  print_vring_layout(layout, options.iova, mapped_size);

  AnonymousBuffer buffer(mapped_size);
  buffer.zero();

  constexpr std::uint32_t dma_flags =
      VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
  std::println("DMA permissions: {}", dma_flag_names(dma_flags));

  DmaMapping dma(vfio.container.get(), buffer.data(), options.iova, mapped_size,
                 dma_flags);
  std::println("Mapped zeroed vring buffer {:p} to IOVA 0x{:x} ({} bytes)",
               static_cast<void *>(buffer.data()), dma.iova(), dma.size());

  DeviceResetGuard reset_guard(common_mapping, mapping_delta);

  program_queue_addresses(common_mapping, mapping_delta, layout, options.iova);
  std::println("Wrote queue_size, queue_desc, queue_avail, queue_used");

  QueueView after_program = read_queue_view(common_mapping, mapping_delta);
  print_queue_view("Selected queue after programming", after_program);

  enable_queue(common_mapping, mapping_delta);
  std::println("Wrote queue_enable=1");

  QueueView after_enable = read_queue_view(common_mapping, mapping_delta);
  print_queue_view("Selected queue after enable", after_enable);
  if (after_enable.enable != 1)
    throw std::runtime_error("queue_enable did not read back as 1");

  std::size_t notify_delta = 0;
  std::uint64_t notify_bar_offset = 0;
  off_t notify_mmap_offset = 0;
  MappedRegion notify_mapping = map_notify_window(
      vfio.device.get(), notify, notify_region, after_enable.notify_off,
      notify_delta, notify_bar_offset, notify_mmap_offset);
  std::println("Notify write target:");
  std::println("  BAR{} relative offset: 0x{:x}", notify.bar,
               notify_bar_offset);
  std::println("  VFIO device-fd file offset: 0x{:x}",
               notify_mmap_offset + static_cast<off_t>(notify_delta));
  std::println("  mmap file offset: 0x{:x}", notify_mmap_offset);
  std::println("  mmap delta: 0x{:x}", notify_delta);

  std::uint16_t notify_value = after_enable.selected;
  if (after_enable.notify_data) {
    std::println("  queue_notify_data candidate: {}",
                 *after_enable.notify_data);
  }
  std::println("  notify value: {} (queue index; "
               "VIRTIO_F_NOTIFICATION_DATA was not negotiated)",
               notify_value);
  write_notify(notify_mapping, notify_delta, notify_value);
  std::println("Wrote one 16-bit notify value");

  selection.restore();
  std::println("Restored queue_select to {}", original_select);

  reset_device(common_mapping, mapping_delta, "after cleanup reset");
  reset_guard.disarm();

  std::uint64_t unmapped = dma.unmap();
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped",
               options.iova, unmapped);
  std::println("Did not write DRIVER_OK.");
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
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

    Options options;
    for (int i = 2; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "--queue" && i + 1 < argc) {
        options.queue = parse_u16(argv[++i], "queue");
      } else if (arg == "--queue-size" && i + 1 < argc) {
        options.queue_size = parse_u16(argv[++i], "queue-size");
      } else if (arg == "--iova" && i + 1 < argc) {
        options.iova = parse_ull(argv[++i], "iova");
      } else if (arg == "--align" && i + 1 < argc) {
        options.align = static_cast<std::size_t>(parse_ull(argv[++i], "align"));
      } else if (arg == "--dry-run") {
        options.yes = false;
      } else if (arg == "--yes") {
        options.yes = true;
      } else {
        usage(argv[0]);
        return 1;
      }
    }

    if (!is_power_of_two(options.align) || options.align < kVringUsedAlignSize) {
      throw std::runtime_error(
          "align must be a power of two and at least 4 bytes");
    }

    if (options.yes)
      run_notify_write(bdf, dev_dir, options);
    else
      dry_run(bdf, dev_dir, options);

    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
