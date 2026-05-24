// Exercise 14: Virtio feature bits through VFIO
//
// Goal: read virtio feature words and practice the FEATURES_OK step without
// starting the device.
//
// Background
// ----------
// Modern virtio devices expose feature registers in COMMON_CFG as selected
// 32-bit windows:
//
//   write device_feature_select = N, read device_feature
//   write guest_feature_select  = N, write guest_feature
//
// A real driver resets the device, sets ACKNOWLEDGE|DRIVER, reads device
// features, writes the subset of features it supports, then sets FEATURES_OK.
// If the device accepts the selection, FEATURES_OK remains set in
// device_status.  DRIVER_OK is a later step and starts real operation after
// queues are configured.
//
// This sample stops after FEATURES_OK.  It does not configure queues, notify
// the device, set DRIVER_OK, or start DMA.
//
// New concepts
// ------------
// - device_feature_select/device_feature selected windows
// - guest_feature_select/guest_feature selected windows
// - minimal modern virtio feature negotiation
// - FEATURES_OK acceptance check

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

// linux/virtio_net.h contains a C field named "class", so this C++ sample
// keeps the virtio-net feature bit numbers local instead of including it.
constexpr unsigned VIRTIO_NET_F_CSUM = 0;
constexpr unsigned VIRTIO_NET_F_GUEST_CSUM = 1;
constexpr unsigned VIRTIO_NET_F_CTRL_GUEST_OFFLOADS = 2;
constexpr unsigned VIRTIO_NET_F_MTU = 3;
constexpr unsigned VIRTIO_NET_F_MAC = 5;
constexpr unsigned VIRTIO_NET_F_GSO = 6;
constexpr unsigned VIRTIO_NET_F_GUEST_TSO4 = 7;
constexpr unsigned VIRTIO_NET_F_GUEST_TSO6 = 8;
constexpr unsigned VIRTIO_NET_F_GUEST_ECN = 9;
constexpr unsigned VIRTIO_NET_F_GUEST_UFO = 10;
constexpr unsigned VIRTIO_NET_F_HOST_TSO4 = 11;
constexpr unsigned VIRTIO_NET_F_HOST_TSO6 = 12;
constexpr unsigned VIRTIO_NET_F_HOST_ECN = 13;
constexpr unsigned VIRTIO_NET_F_HOST_UFO = 14;
constexpr unsigned VIRTIO_NET_F_MRG_RXBUF = 15;
constexpr unsigned VIRTIO_NET_F_STATUS = 16;
constexpr unsigned VIRTIO_NET_F_CTRL_VQ = 17;
constexpr unsigned VIRTIO_NET_F_CTRL_RX = 18;
constexpr unsigned VIRTIO_NET_F_CTRL_VLAN = 19;
constexpr unsigned VIRTIO_NET_F_CTRL_RX_EXTRA = 20;
constexpr unsigned VIRTIO_NET_F_GUEST_ANNOUNCE = 21;
constexpr unsigned VIRTIO_NET_F_MQ = 22;
constexpr unsigned VIRTIO_NET_F_CTRL_MAC_ADDR = 23;
constexpr unsigned VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_MAPPED = 46;
constexpr unsigned VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_CSUM_MAPPED = 47;
constexpr unsigned VIRTIO_NET_F_DEVICE_STATS = 50;
constexpr unsigned VIRTIO_NET_F_VQ_NOTF_COAL = 52;
constexpr unsigned VIRTIO_NET_F_NOTF_COAL = 53;
constexpr unsigned VIRTIO_NET_F_GUEST_USO4 = 54;
constexpr unsigned VIRTIO_NET_F_GUEST_USO6 = 55;
constexpr unsigned VIRTIO_NET_F_HOST_USO = 56;
constexpr unsigned VIRTIO_NET_F_HASH_REPORT = 57;
constexpr unsigned VIRTIO_NET_F_GUEST_HDRLEN = 59;
constexpr unsigned VIRTIO_NET_F_RSS = 60;
constexpr unsigned VIRTIO_NET_F_RSC_EXT = 61;
constexpr unsigned VIRTIO_NET_F_STANDBY = 62;
constexpr unsigned VIRTIO_NET_F_SPEED_DUPLEX = 63;
constexpr unsigned VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO = 65;
constexpr unsigned VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_CSUM = 66;
constexpr unsigned VIRTIO_NET_F_HOST_UDP_TUNNEL_GSO = 67;
constexpr unsigned VIRTIO_NET_F_HOST_UDP_TUNNEL_GSO_CSUM = 68;

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

  std::uint32_t read32(std::size_t offset) const {
    check_range(offset, sizeof(std::uint32_t));
    return *reinterpret_cast<volatile const std::uint32_t *>(base_ + offset);
  }

  void write8(std::size_t offset, std::uint8_t value) {
    check_range(offset, sizeof(std::uint8_t));
    *(base_ + offset) = value;
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
};

struct FeatureWords {
  std::uint32_t original_select = 0;
  std::vector<std::uint32_t> words;
};

struct FeatureName {
  unsigned bit = 0;
  std::string_view name;
};

static constexpr std::array kFeatureNames = {
    FeatureName{VIRTIO_NET_F_CSUM, "VIRTIO_NET_F_CSUM"},
    FeatureName{VIRTIO_NET_F_GUEST_CSUM, "VIRTIO_NET_F_GUEST_CSUM"},
    FeatureName{VIRTIO_NET_F_CTRL_GUEST_OFFLOADS,
                "VIRTIO_NET_F_CTRL_GUEST_OFFLOADS"},
    FeatureName{VIRTIO_NET_F_MTU, "VIRTIO_NET_F_MTU"},
    FeatureName{VIRTIO_NET_F_MAC, "VIRTIO_NET_F_MAC"},
    FeatureName{VIRTIO_NET_F_GSO, "VIRTIO_NET_F_GSO"},
    FeatureName{VIRTIO_NET_F_GUEST_TSO4, "VIRTIO_NET_F_GUEST_TSO4"},
    FeatureName{VIRTIO_NET_F_GUEST_TSO6, "VIRTIO_NET_F_GUEST_TSO6"},
    FeatureName{VIRTIO_NET_F_GUEST_ECN, "VIRTIO_NET_F_GUEST_ECN"},
    FeatureName{VIRTIO_NET_F_GUEST_UFO, "VIRTIO_NET_F_GUEST_UFO"},
    FeatureName{VIRTIO_NET_F_HOST_TSO4, "VIRTIO_NET_F_HOST_TSO4"},
    FeatureName{VIRTIO_NET_F_HOST_TSO6, "VIRTIO_NET_F_HOST_TSO6"},
    FeatureName{VIRTIO_NET_F_HOST_ECN, "VIRTIO_NET_F_HOST_ECN"},
    FeatureName{VIRTIO_NET_F_HOST_UFO, "VIRTIO_NET_F_HOST_UFO"},
    FeatureName{VIRTIO_NET_F_MRG_RXBUF, "VIRTIO_NET_F_MRG_RXBUF"},
    FeatureName{VIRTIO_NET_F_STATUS, "VIRTIO_NET_F_STATUS"},
    FeatureName{VIRTIO_NET_F_CTRL_VQ, "VIRTIO_NET_F_CTRL_VQ"},
    FeatureName{VIRTIO_NET_F_CTRL_RX, "VIRTIO_NET_F_CTRL_RX"},
    FeatureName{VIRTIO_NET_F_CTRL_VLAN, "VIRTIO_NET_F_CTRL_VLAN"},
    FeatureName{VIRTIO_NET_F_CTRL_RX_EXTRA,
                "VIRTIO_NET_F_CTRL_RX_EXTRA"},
    FeatureName{VIRTIO_NET_F_GUEST_ANNOUNCE,
                "VIRTIO_NET_F_GUEST_ANNOUNCE"},
    FeatureName{VIRTIO_NET_F_MQ, "VIRTIO_NET_F_MQ"},
    FeatureName{VIRTIO_NET_F_CTRL_MAC_ADDR, "VIRTIO_NET_F_CTRL_MAC_ADDR"},
    FeatureName{VIRTIO_F_NOTIFY_ON_EMPTY, "VIRTIO_F_NOTIFY_ON_EMPTY"},
    FeatureName{VIRTIO_F_ANY_LAYOUT, "VIRTIO_F_ANY_LAYOUT"},
    FeatureName{VIRTIO_F_VERSION_1, "VIRTIO_F_VERSION_1"},
    FeatureName{VIRTIO_F_ACCESS_PLATFORM, "VIRTIO_F_ACCESS_PLATFORM"},
    FeatureName{VIRTIO_F_RING_PACKED, "VIRTIO_F_RING_PACKED"},
    FeatureName{VIRTIO_F_IN_ORDER, "VIRTIO_F_IN_ORDER"},
    FeatureName{VIRTIO_F_ORDER_PLATFORM, "VIRTIO_F_ORDER_PLATFORM"},
    FeatureName{VIRTIO_F_SR_IOV, "VIRTIO_F_SR_IOV"},
    FeatureName{VIRTIO_F_NOTIFICATION_DATA, "VIRTIO_F_NOTIFICATION_DATA"},
    FeatureName{VIRTIO_F_NOTIF_CONFIG_DATA, "VIRTIO_F_NOTIF_CONFIG_DATA"},
    FeatureName{VIRTIO_F_RING_RESET, "VIRTIO_F_RING_RESET"},
    FeatureName{VIRTIO_F_ADMIN_VQ, "VIRTIO_F_ADMIN_VQ"},
    FeatureName{VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_MAPPED,
                "VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_MAPPED"},
    FeatureName{VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_CSUM_MAPPED,
                "VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_CSUM_MAPPED"},
    FeatureName{VIRTIO_NET_F_DEVICE_STATS, "VIRTIO_NET_F_DEVICE_STATS"},
    FeatureName{VIRTIO_NET_F_VQ_NOTF_COAL, "VIRTIO_NET_F_VQ_NOTF_COAL"},
    FeatureName{VIRTIO_NET_F_NOTF_COAL, "VIRTIO_NET_F_NOTF_COAL"},
    FeatureName{VIRTIO_NET_F_GUEST_USO4, "VIRTIO_NET_F_GUEST_USO4"},
    FeatureName{VIRTIO_NET_F_GUEST_USO6, "VIRTIO_NET_F_GUEST_USO6"},
    FeatureName{VIRTIO_NET_F_HOST_USO, "VIRTIO_NET_F_HOST_USO"},
    FeatureName{VIRTIO_NET_F_HASH_REPORT, "VIRTIO_NET_F_HASH_REPORT"},
    FeatureName{VIRTIO_NET_F_GUEST_HDRLEN, "VIRTIO_NET_F_GUEST_HDRLEN"},
    FeatureName{VIRTIO_NET_F_RSS, "VIRTIO_NET_F_RSS"},
    FeatureName{VIRTIO_NET_F_RSC_EXT, "VIRTIO_NET_F_RSC_EXT"},
    FeatureName{VIRTIO_NET_F_STANDBY, "VIRTIO_NET_F_STANDBY"},
    FeatureName{VIRTIO_NET_F_SPEED_DUPLEX, "VIRTIO_NET_F_SPEED_DUPLEX"},
    FeatureName{VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO,
                "VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO"},
    FeatureName{VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_CSUM,
                "VIRTIO_NET_F_GUEST_UDP_TUNNEL_GSO_CSUM"},
    FeatureName{VIRTIO_NET_F_HOST_UDP_TUNNEL_GSO,
                "VIRTIO_NET_F_HOST_UDP_TUNNEL_GSO"},
    FeatureName{VIRTIO_NET_F_HOST_UDP_TUNNEL_GSO_CSUM,
                "VIRTIO_NET_F_HOST_UDP_TUNNEL_GSO_CSUM"},
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
               "  {} <BDF> --show [--yes]\n"
               "  {} <BDF> --negotiate-minimal [--yes]\n\n"
               "Defaults:\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.6 --show\n"
               "  {} c1:00.6 --show --yes\n"
               "  {} c1:00.6 --negotiate-minimal --yes",
               argv0, argv0, argv0, argv0, argv0);
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

static void print_status(std::string_view label, std::uint8_t status) {
  std::println("{}: 0x{:02x} ({})", label, status, status_names(status));
}

static std::string_view feature_name(unsigned bit) {
  for (const FeatureName &feature : kFeatureNames) {
    if (feature.bit == bit)
      return feature.name;
  }
  return "unknown";
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

  std::println("  decoded set bits:");
  bool any = false;
  for (std::size_t word = 0; word < words.size(); ++word) {
    for (unsigned shift = 0; shift < 32; ++shift) {
      if (!(words[word] & (1u << shift)))
        continue;
      unsigned bit = static_cast<unsigned>(word * 32 + shift);
      std::println("    bit {:2}: {}", bit, feature_name(bit));
      any = true;
    }
  }

  if (!any)
    std::println("    <none>");
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
  if (common.length < VIRTIO_PCI_COMMON_STATUS + sizeof(std::uint8_t))
    throw std::runtime_error("COMMON_CFG is shorter than feature/status fields");

  std::uint64_t available = info.size - common.offset;
  std::size_t common_length =
      static_cast<std::size_t>(std::min<std::uint64_t>(common.length, available));
  std::size_t inspect_length =
      std::min<std::size_t>(common_length, VIRTIO_PCI_COMMON_STATUS + 1);

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

static std::uint8_t read_status(const MappedRegion &mapping,
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

  throw std::runtime_error("timed out waiting for device_status " +
                           status_names(expected));
}

static void print_current_feature_view(const MappedRegion &mapping,
                                       std::size_t mapping_delta) {
  std::uint32_t df_select =
      mapping.read32(mapping_delta + VIRTIO_PCI_COMMON_DFSELECT);
  std::uint32_t device_feature =
      mapping.read32(mapping_delta + VIRTIO_PCI_COMMON_DF);
  std::uint32_t gf_select =
      mapping.read32(mapping_delta + VIRTIO_PCI_COMMON_GFSELECT);
  std::uint32_t guest_feature =
      mapping.read32(mapping_delta + VIRTIO_PCI_COMMON_GF);

  std::println("Current selected feature view:");
  std::println("  device_feature_select: {}",
               static_cast<unsigned long long>(df_select));
  std::println("  device_feature: 0x{:08x}", device_feature);
  std::println("  guest_feature_select: {}",
               static_cast<unsigned long long>(gf_select));
  std::println("  guest_feature: 0x{:08x}", guest_feature);
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

static void show_features(MappedRegion &mapping, std::size_t mapping_delta,
                          bool yes) {
  print_status("device_status", read_status(mapping, mapping_delta));
  print_current_feature_view(mapping, mapping_delta);

  if (!yes) {
    std::println(
        "Would write device_feature_select=0..{} and "
        "guest_feature_select=0..{} to enumerate feature words.",
        kFeatureWords - 1, kFeatureWords - 1);
    std::println("Pass --yes to perform these selector writes.");
    return;
  }

  FeatureWords device_features =
      read_feature_words(mapping, mapping_delta, VIRTIO_PCI_COMMON_DFSELECT,
                         VIRTIO_PCI_COMMON_DF, kFeatureWords);
  FeatureWords guest_features =
      read_feature_words(mapping, mapping_delta, VIRTIO_PCI_COMMON_GFSELECT,
                         VIRTIO_PCI_COMMON_GF, kFeatureWords);

  print_feature_words("Device feature words", device_features.words);
  print_feature_words("Guest feature words", guest_features.words);
  std::println("Restored device_feature_select to {}",
               device_features.original_select);
  std::println("Restored guest_feature_select to {}",
               guest_features.original_select);
}

static void negotiate_minimal(MappedRegion &mapping, std::size_t mapping_delta,
                              bool yes) {
  std::uint8_t initial = read_status(mapping, mapping_delta);
  print_status("initial device_status", initial);
  print_current_feature_view(mapping, mapping_delta);

  if (!yes) {
    std::println("Would write device_status=0 and wait for reset");
    std::println("Would write ACKNOWLEDGE, then ACKNOWLEDGE|DRIVER");
    std::println("Would enumerate device feature words 0..{}",
                 kFeatureWords - 1);
    std::println(
        "Would write guest features: VIRTIO_F_VERSION_1 plus "
        "VIRTIO_F_ACCESS_PLATFORM if offered");
    std::println("Would write FEATURES_OK and verify the bit remains set");
    std::println("Would stop before DRIVER_OK; queues and DMA stay disabled.");
    std::println("Pass --yes to perform these feature/status writes.");
    return;
  }

  write_status(mapping, mapping_delta, 0);
  wait_for_status(mapping, mapping_delta, 0, std::chrono::milliseconds(1000));
  print_status("after reset", read_status(mapping, mapping_delta));

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

  std::println("Stopped before DRIVER_OK; queues and DMA are still disabled.");
}

enum class Mode {
  Show,
  NegotiateMinimal,
};

static void run_feature_sample(const std::string &bdf, const fs::path &dev_dir,
                               Mode mode, bool yes) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);

  VfioContext vfio = open_vfio_context(bdf, dev_dir);
  ConfigRegion config = get_config_region(vfio.device.get());
  VirtioCap common = find_common_cfg(read_virtio_caps(vfio.device.get(), config));

  std::size_t mapping_delta = 0;
  bool writable = yes;
  MappedRegion mapping =
      map_common_cfg(vfio.device.get(), common, mapping_delta, writable);

  if (mode == Mode::Show)
    show_features(mapping, mapping_delta, yes);
  else
    negotiate_minimal(mapping, mapping_delta, yes);
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
    bool negotiate_minimal_mode = false;
    bool yes = false;

    for (int i = 2; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "--show") {
        show = true;
      } else if (arg == "--negotiate-minimal") {
        negotiate_minimal_mode = true;
      } else if (arg == "--dry-run") {
        yes = false;
      } else if (arg == "--yes") {
        yes = true;
      } else {
        usage(argv[0]);
        return 1;
      }
    }

    if (show == negotiate_minimal_mode) {
      usage(argv[0]);
      return 1;
    }

    Mode mode = show ? Mode::Show : Mode::NegotiateMinimal;
    run_feature_sample(bdf, dev_dir, mode, yes);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
