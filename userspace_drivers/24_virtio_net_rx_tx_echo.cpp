// Exercise 24: Receive one virtio-net packet and echo it through VFIO
//
// Goal: configure the first virtio-net RX/TX queue pair, wait for one RX
// packet, build a reply frame from it, publish one TX descriptor, notify TX,
// and observe TX used-ring completion.
//
// Background
// ----------
// Exercise 22 proved that userspace can receive packets once PCI bus mastering
// and RX buffers are in place.  Exercise 23 proved that userspace can transmit
// one packet.  This sample ties both directions together:
//
//   reset -> ACKNOWLEDGE -> DRIVER -> FEATURES_OK
//   configure RX queue 0
//   configure TX queue 1
//   fill RX descriptors with writable packet buffers
//   publish RX descriptors through avail.idx
//   enable both queues
//   write DRIVER_OK
//   notify RX queue
//   wait for one RX used entry
//   copy the received Ethernet frame into a TX buffer
//   swap Ethernet source/destination and set source to this VF's MAC
//   publish one TX descriptor through avail.idx
//   notify TX queue
//   poll TX used.idx until the device completes the packet
//
// It does not keep the link running or recycle descriptors.  Before unmapping
// DMA memory, it resets the device so the device stops using the queues.
//
// New concepts
// ------------
// - Consuming one RX used-ring entry in userspace
// - Building a TX reply from a received Ethernet frame
// - Publishing a device-readable TX descriptor after DRIVER_OK
// - Notifying TX and waiting for TX used-ring completion

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <format>
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

#include "vfio_utils.hpp"

namespace fs = std::filesystem;

static_assert(std::endian::native == std::endian::little,
              "this tutorial sample expects a little-endian host");

constexpr std::size_t kFeatureWords = 3;
constexpr std::size_t kVringDescSize = 16;
constexpr std::size_t kVringUsedElemSize = 8;
constexpr std::size_t kVringUsedAlignSize = 4;
constexpr std::uint16_t kDefaultRxQueue = 0;
constexpr std::uint16_t kDefaultTxQueue = 1;
constexpr std::uint16_t kDefaultRxBuffers = 64;
constexpr std::size_t kDefaultRxBufferSize = 2048;
constexpr std::size_t kDefaultDumpBytes = 160;
constexpr std::size_t kVirtioNetHeaderSize = 12;
constexpr std::uint16_t kDefaultEthertype = 0x88b5;
constexpr std::size_t kEthernetHeaderSize = 14;
constexpr std::size_t kEthernetMinFrameSize = 60;
constexpr std::uint16_t kVringDescFWrite = 2;

// linux/virtio_net.h contains a C field named "class", so this C++ sample
// keeps the few virtio-net feature bit numbers and config offsets it needs
// local instead of including that header.
constexpr unsigned VIRTIO_NET_F_MAC = 5;
constexpr unsigned VIRTIO_NET_F_STATUS = 16;
constexpr std::size_t kVirtioNetConfigMac = 0;
constexpr std::size_t kVirtioNetConfigStatus = 6;
constexpr std::size_t kVirtioNetConfigMaxVirtqueuePairs = 8;
constexpr std::size_t kVirtioNetConfigMtu = 10;
constexpr std::size_t kVirtioNetConfigMinBytes = 12;

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

struct VringDesc {
  std::uint64_t addr = 0;
  std::uint32_t len = 0;
  std::uint16_t flags = 0;
  std::uint16_t next = 0;
};

struct VringUsedElem {
  std::uint32_t id = 0;
  std::uint32_t len = 0;
};

static_assert(sizeof(VringDesc) == kVringDescSize);
static_assert(sizeof(VringUsedElem) == kVringUsedElemSize);

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

struct VirtioNetConfigView {
  std::array<std::uint8_t, 6> mac{};
  std::optional<std::uint16_t> status;
  std::optional<std::uint16_t> max_virtqueue_pairs;
  std::optional<std::uint16_t> mtu;
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

struct DmaLayout {
  VringLayout rx_vring;
  VringLayout tx_vring;
  std::size_t rx_vring_offset = 0;
  std::size_t tx_vring_offset = 0;
  std::size_t rx_buffers_offset = 0;
  std::size_t rx_buffers_size = 0;
  std::size_t tx_packet_offset = 0;
  std::size_t tx_packet_size = 0;
  std::size_t total_size = 0;
};

struct Options {
  std::uint16_t rx_queue = kDefaultRxQueue;
  std::uint16_t tx_queue = kDefaultTxQueue;
  std::optional<std::uint16_t> queue_size;
  std::uint64_t iova = 0x100000000ull;
  std::size_t align = 4096;
  std::uint16_t rx_buffers = kDefaultRxBuffers;
  std::size_t rx_buffer_size = kDefaultRxBufferSize;
  std::uint32_t wait_ms = 30000;
  std::size_t dump_bytes = kDefaultDumpBytes;
  std::optional<std::array<std::uint8_t, 6>> src_mac;
  std::optional<std::uint16_t> match_ethertype = kDefaultEthertype;
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
               "  {} <BDF> [--rx-queue <n>] [--tx-queue <n>]\n"
               "       [--queue-size <n>] [--rx-buffers <n>]\n"
               "       [--rx-buffer-size <bytes>] [--iova <addr>]\n"
               "       [--align <bytes>] [--wait-ms <n>]\n"
               "       [--src-mac <mac>]\n"
               "       [--match-ethertype <hex>|--accept-any-ethertype]\n"
               "       [--dump-bytes <n>]\n"
               "       [--dry-run|--yes]\n\n"
               "Defaults:\n"
               "  rx-queue = 0\n"
               "  tx-queue = 1\n"
               "  queue-size = each target queue's device-reported size\n"
               "  rx-buffers = 64\n"
               "  rx-buffer-size = 2048\n"
               "  iova = 0x100000000\n"
               "  align = 4096\n"
               "  wait-ms = 30000\n"
               "  src-mac = virtio-net config MAC\n"
               "  match-ethertype = 0x88b5\n"
               "  dump-bytes = 160\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.6\n"
               "  {} c1:00.6 --wait-ms 60000 --yes\n"
               "  {} c1:00.6 --match-ethertype 0x88b5 --dump-bytes 192 --yes",
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

static std::array<std::uint8_t, 6> parse_mac(std::string_view text,
                                             std::string_view name) {
  std::string hex;
  hex.reserve(12);
  for (char ch : text) {
    if (ch == ':' || ch == '-')
      continue;
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      throw std::runtime_error("invalid " + std::string(name) + ": " +
                               std::string(text));
    }
    hex.push_back(ch);
  }
  if (hex.size() != 12) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }

  std::array<std::uint8_t, 6> mac{};
  for (std::size_t i = 0; i < mac.size(); ++i) {
    std::string byte_text = hex.substr(i * 2, 2);
    mac[i] = static_cast<std::uint8_t>(std::stoul(byte_text, nullptr, 16));
  }
  return mac;
}

static std::uint32_t parse_u32(std::string_view text, std::string_view name) {
  unsigned long long value = parse_ull(text, name);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }
  return static_cast<std::uint32_t>(value);
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

static std::size_t checked_add(std::size_t a, std::size_t b,
                               std::string_view what) {
  if (a > std::numeric_limits<std::size_t>::max() - b)
    throw std::runtime_error(std::string(what) + " size overflow");
  return a + b;
}

static std::size_t checked_mul(std::size_t a, std::size_t b,
                               std::string_view what) {
  if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
    throw std::runtime_error(std::string(what) + " size overflow");
  return a * b;
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

static std::string pci_command_names(std::uint16_t command) {
  std::vector<std::string_view> names;
  if (command & PCI_COMMAND_IO)
    names.push_back("IO");
  if (command & PCI_COMMAND_MEMORY)
    names.push_back("MEMORY");
  if (command & PCI_COMMAND_MASTER)
    names.push_back("BUS_MASTER");
  if (command & PCI_COMMAND_INTX_DISABLE)
    names.push_back("INTX_DISABLE");
  return join_flags(names);
}

static std::string mac_string(const std::array<std::uint8_t, 6> &mac) {
  return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0],
                     mac[1], mac[2], mac[3], mac[4], mac[5]);
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

static void pwrite_exact(int fd, const void *data, std::size_t size,
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
      throw std::runtime_error(std::string(what) + ": " + errno_text(error));
    }
    if (wrote == 0)
      throw std::runtime_error(std::string(what) + ": short write");
    done += static_cast<std::size_t>(wrote);
  }
}

static void read_config_bytes(int device_fd, const ConfigRegion &config,
                              std::uint16_t offset, void *data,
                              std::size_t size) {
  if (static_cast<std::uint64_t>(offset) + size > config.size)
    throw std::runtime_error("PCI config read past VFIO CONFIG region");
  pread_exact(device_fd, data, size, config.offset + offset, "pread CONFIG");
}

static void write_config_bytes(int device_fd, const ConfigRegion &config,
                               std::uint16_t offset, const void *data,
                               std::size_t size) {
  if (static_cast<std::uint64_t>(offset) + size > config.size)
    throw std::runtime_error("PCI config write past VFIO CONFIG region");
  pwrite_exact(device_fd, data, size, config.offset + offset, "pwrite CONFIG");
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

static void write_config_le16(int device_fd, const ConfigRegion &config,
                              std::uint16_t offset, std::uint16_t value) {
  std::uint8_t bytes[2]{
      static_cast<std::uint8_t>(value & 0xffu),
      static_cast<std::uint8_t>(value >> 8),
  };
  write_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
}

class PciCommandGuard {
public:
  PciCommandGuard(int device_fd, ConfigRegion config)
      : device_fd_(device_fd), config_(config),
        original_(read_config_le16(device_fd_, config_, PCI_COMMAND)) {
    current_ = original_;
    std::println("PCI command before: 0x{:04x} ({})", original_,
                 pci_command_names(original_));
  }

  PciCommandGuard(const PciCommandGuard &) = delete;
  PciCommandGuard &operator=(const PciCommandGuard &) = delete;

  ~PciCommandGuard() { restore_noexcept(); }

  void enable_bus_mastering() {
    std::uint16_t wanted = original_ | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    if (wanted == current_) {
      std::println("PCI command already allows MMIO and bus mastering");
      return;
    }

    write_config_le16(device_fd_, config_, PCI_COMMAND, wanted);
    current_ = read_config_le16(device_fd_, config_, PCI_COMMAND);
    std::println("PCI command after enabling DMA: 0x{:04x} ({})", current_,
                 pci_command_names(current_));
    if (!(current_ & PCI_COMMAND_MASTER))
      throw std::runtime_error("PCI bus mastering did not stay enabled");
    if (!(current_ & PCI_COMMAND_MEMORY))
      throw std::runtime_error("PCI memory space did not stay enabled");
  }

  void restore() {
    if (restored_ || current_ == original_) {
      restored_ = true;
      return;
    }

    write_config_le16(device_fd_, config_, PCI_COMMAND, original_);
    current_ = read_config_le16(device_fd_, config_, PCI_COMMAND);
    restored_ = true;
    std::println("Restored PCI command to 0x{:04x} ({})", current_,
                 pci_command_names(current_));
  }

private:
  void restore_noexcept() noexcept {
    try {
      restore();
    } catch (...) {
    }
  }

  int device_fd_ = -1;
  ConfigRegion config_{};
  std::uint16_t original_ = 0;
  std::uint16_t current_ = 0;
  bool restored_ = false;
};

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

static VirtioCap find_device_cfg(const std::vector<VirtioCap> &caps) {
  for (const VirtioCap &cap : caps) {
    if (cap.cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG)
      return cap;
  }
  throw std::runtime_error("virtio DEVICE_CFG capability was not found");
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

static MappedRegion map_device_cfg(int device_fd, const VirtioCap &device_cfg,
                                   std::size_t &mapping_delta) {
  __u32 region_index = vfio_bar_region_index(device_cfg.bar);
  RegionInfo region = get_region_info(device_fd, region_index);
  const vfio_region_info &info = region.info;

  if (info.size == 0)
    throw std::runtime_error("DEVICE_CFG BAR region is not present");
  if (!(info.flags & VFIO_REGION_INFO_FLAG_MMAP))
    throw std::runtime_error("DEVICE_CFG BAR region does not support mmap");
  if (device_cfg.offset >= info.size)
    throw std::runtime_error("DEVICE_CFG offset is past BAR region size");
  if (device_cfg.length < kVirtioNetConfigMinBytes)
    throw std::runtime_error("DEVICE_CFG is shorter than virtio-net config");

  std::uint64_t available = info.size - device_cfg.offset;
  std::size_t inspect_length = static_cast<std::size_t>(
      std::min<std::uint64_t>(device_cfg.length, available));

  if (!region.mmap_areas.empty()) {
    bool inside_sparse_area = false;
    for (const MmapArea &area : region.mmap_areas) {
      if (area.contains(device_cfg.offset, inspect_length)) {
        inside_sparse_area = true;
        break;
      }
    }
    if (!inside_sparse_area) {
      throw std::runtime_error(
          "DEVICE_CFG range is not fully inside a VFIO sparse mmap area");
    }
  }

  long page_size = system_page_size();
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

static VirtioNetConfigView read_virtio_net_config(MappedRegion &mapping,
                                                  std::size_t mapping_delta,
                                                  std::uint64_t cfg_length) {
  VirtioNetConfigView view;

  if (cfg_length >= kVirtioNetConfigMac + view.mac.size()) {
    for (std::size_t i = 0; i < view.mac.size(); ++i)
      view.mac[i] = mapping.read8(mapping_delta + kVirtioNetConfigMac + i);
  }
  if (cfg_length >= kVirtioNetConfigStatus + sizeof(std::uint16_t)) {
    view.status = mapping.read16(mapping_delta + kVirtioNetConfigStatus);
  }
  if (cfg_length >=
      kVirtioNetConfigMaxVirtqueuePairs + sizeof(std::uint16_t)) {
    view.max_virtqueue_pairs =
        mapping.read16(mapping_delta + kVirtioNetConfigMaxVirtqueuePairs);
  }
  if (cfg_length >= kVirtioNetConfigMtu + sizeof(std::uint16_t)) {
    view.mtu = mapping.read16(mapping_delta + kVirtioNetConfigMtu);
  }

  return view;
}

static void print_virtio_net_config(std::string_view label,
                                    const VirtioNetConfigView &view,
                                    const std::vector<std::uint32_t> &features) {
  std::println("{}:", label);
  if (feature_is_set(features, VIRTIO_NET_F_MAC))
    std::println("  mac: {}", mac_string(view.mac));
  else
    std::println("  mac: {} (feature not negotiated)", mac_string(view.mac));

  if (view.status) {
    std::println("  status: 0x{:04x}{}", *view.status,
                 (*view.status & 1u) ? " (LINK_UP)" : "");
  } else {
    std::println("  status: <not present>");
  }
  if (view.max_virtqueue_pairs)
    std::println("  max_virtqueue_pairs: {}", *view.max_virtqueue_pairs);
  if (view.mtu)
    std::println("  mtu: {}", *view.mtu);
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
  if (feature_is_set(device_features, VIRTIO_NET_F_MAC))
    set_feature(guest, VIRTIO_NET_F_MAC);
  if (feature_is_set(device_features, VIRTIO_NET_F_STATUS))
    set_feature(guest, VIRTIO_NET_F_STATUS);

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

static std::vector<std::uint32_t>
negotiate_minimal_features(MappedRegion &mapping, std::size_t mapping_delta) {
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
  print_feature_words("Guest feature words", guest_features);
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
  return guest_features;
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

static DmaLayout compute_dma_layout(std::uint16_t rx_queue_size,
                                    std::uint16_t tx_queue_size,
                                    std::size_t align,
                                    std::uint16_t rx_buffers,
                                    std::size_t rx_buffer_size,
                                    std::size_t tx_packet_size) {
  if (rx_buffers == 0)
    throw std::runtime_error("rx-buffers must be greater than 0");
  if (rx_buffers > rx_queue_size) {
    throw std::runtime_error("rx-buffers must not exceed RX queue_size " +
                             std::to_string(rx_queue_size));
  }
  if (rx_buffer_size == 0)
    throw std::runtime_error("rx-buffer-size must be greater than 0");
  if (tx_packet_size == 0)
    throw std::runtime_error("tx packet size must be greater than 0");

  DmaLayout layout;
  layout.rx_vring = compute_vring_layout(rx_queue_size, align);
  layout.tx_vring = compute_vring_layout(tx_queue_size, align);
  layout.rx_vring_offset = 0;
  layout.tx_vring_offset = round_up_to_page(layout.rx_vring.total_size);
  layout.rx_buffers_offset =
      round_up_to_page(checked_add(layout.tx_vring_offset,
                                   layout.tx_vring.total_size,
                                   "TX vring end"));
  layout.rx_buffers_size =
      checked_mul(rx_buffers, rx_buffer_size, "RX buffer area");
  layout.tx_packet_offset =
      round_up_to_page(checked_add(layout.rx_buffers_offset,
                                   layout.rx_buffers_size,
                                   "RX buffer area end"));
  layout.tx_packet_size = tx_packet_size;
  layout.total_size =
      round_up_to_page(checked_add(layout.tx_packet_offset,
                                   layout.tx_packet_size,
                                   "DMA mapping"));
  return layout;
}

static void print_dma_layout(const DmaLayout &layout, std::uint64_t base_iova,
                             std::uint16_t rx_buffers,
                             std::size_t rx_buffer_size) {
  std::println("DMA memory layout:");
  std::println("  RX vring offset: 0x{:x}", layout.rx_vring_offset);
  std::println("  RX vring IOVA: 0x{:x}", base_iova + layout.rx_vring_offset);
  std::println("  RX vring bytes: {}", layout.rx_vring.total_size);
  std::println("  TX vring offset: 0x{:x}", layout.tx_vring_offset);
  std::println("  TX vring IOVA: 0x{:x}", base_iova + layout.tx_vring_offset);
  std::println("  TX vring bytes: {}", layout.tx_vring.total_size);
  std::println("  RX buffers offset: 0x{:x}", layout.rx_buffers_offset);
  std::println("  RX buffers IOVA: 0x{:x}",
               base_iova + layout.rx_buffers_offset);
  std::println("  RX buffers: {} x {} bytes", rx_buffers, rx_buffer_size);
  std::println("  RX buffer bytes: {}", layout.rx_buffers_size);
  std::println("  TX packet offset: 0x{:x}", layout.tx_packet_offset);
  std::println("  TX packet IOVA: 0x{:x}",
               base_iova + layout.tx_packet_offset);
  std::println("  TX scratch buffer bytes: {}", layout.tx_packet_size);
  std::println("  mapped bytes: {}", layout.total_size);
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

static VringDesc *vring_descs(std::uint8_t *queue_base,
                              const VringLayout &layout) {
  return reinterpret_cast<VringDesc *>(queue_base + layout.desc_offset);
}

static std::uint16_t *vring_avail_ring(std::uint8_t *queue_base,
                                       const VringLayout &layout) {
  return reinterpret_cast<std::uint16_t *>(queue_base + layout.avail_offset +
                                           sizeof(std::uint16_t) * 2);
}

static volatile std::uint16_t *vring_avail_idx(std::uint8_t *queue_base,
                                               const VringLayout &layout) {
  return reinterpret_cast<volatile std::uint16_t *>(
      queue_base + layout.avail_offset + sizeof(std::uint16_t));
}

static volatile std::uint16_t *vring_used_idx(std::uint8_t *queue_base,
                                              const VringLayout &layout) {
  return reinterpret_cast<volatile std::uint16_t *>(
      queue_base + layout.used_offset + sizeof(std::uint16_t));
}

static volatile const VringUsedElem *
vring_used_elems(const std::uint8_t *queue_base, const VringLayout &layout) {
  return reinterpret_cast<volatile const VringUsedElem *>(
      queue_base + layout.used_offset + sizeof(std::uint16_t) * 2);
}

static void publish_rx_buffers(std::uint8_t *rx_queue_base,
                               const VringLayout &rx_layout,
                               std::uint64_t rx_buffer_iova,
                               std::uint16_t rx_buffers,
                               std::size_t rx_buffer_size) {
  VringDesc *desc = vring_descs(rx_queue_base, rx_layout);
  std::uint16_t *avail_ring = vring_avail_ring(rx_queue_base, rx_layout);

  if (rx_buffer_size > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("rx-buffer-size is too large for a vring desc");

  for (std::uint16_t i = 0; i < rx_buffers; ++i) {
    desc[i].addr = rx_buffer_iova + static_cast<std::uint64_t>(i) *
                                      rx_buffer_size;
    desc[i].len = static_cast<std::uint32_t>(rx_buffer_size);
    desc[i].flags = kVringDescFWrite;
    desc[i].next = 0;
    avail_ring[i] = i;
  }

  std::atomic_thread_fence(std::memory_order_release);
  *vring_avail_idx(rx_queue_base, rx_layout) = rx_buffers;
  std::atomic_thread_fence(std::memory_order_release);
}

static void publish_tx_packet(std::uint8_t *tx_queue_base,
                              const VringLayout &tx_layout,
                              std::uint64_t tx_packet_iova,
                              std::size_t tx_packet_size) {
  if (tx_packet_size > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("TX packet is too large for a vring desc");

  VringDesc *desc = vring_descs(tx_queue_base, tx_layout);
  std::uint16_t *avail_ring = vring_avail_ring(tx_queue_base, tx_layout);
  desc[0].addr = tx_packet_iova;
  desc[0].len = static_cast<std::uint32_t>(tx_packet_size);
  desc[0].flags = 0;
  desc[0].next = 0;
  avail_ring[0] = 0;

  std::atomic_thread_fence(std::memory_order_release);
  *vring_avail_idx(tx_queue_base, tx_layout) = 1;
  std::atomic_thread_fence(std::memory_order_release);
}

static std::uint16_t read_used_idx(std::uint8_t *queue_base,
                                   const VringLayout &layout) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return *vring_used_idx(queue_base, layout);
}

static std::uint16_t used_delta(std::uint16_t before, std::uint16_t after) {
  return static_cast<std::uint16_t>(after - before);
}

static std::uint16_t read_le16_from_bytes(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

static std::uint16_t read_be16_from_bytes(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(bytes[1]) |
         (static_cast<std::uint16_t>(bytes[0]) << 8);
}

static std::array<std::uint8_t, 6> mac_from_bytes(const std::uint8_t *bytes) {
  std::array<std::uint8_t, 6> mac{};
  std::copy(bytes, bytes + mac.size(), mac.begin());
  return mac;
}

static std::size_t tx_reply_size_for_rx(std::size_t rx_used_len) {
  if (rx_used_len < kVirtioNetHeaderSize + kEthernetHeaderSize) {
    throw std::runtime_error(
        "received buffer is too short for virtio-net header plus Ethernet");
  }

  std::size_t rx_frame_size = rx_used_len - kVirtioNetHeaderSize;
  return kVirtioNetHeaderSize +
         std::max(rx_frame_size, kEthernetMinFrameSize);
}

static std::size_t build_tx_reply_from_rx(
    std::uint8_t *tx_packet, std::size_t tx_buffer_size,
    const std::uint8_t *rx_buffer, std::size_t rx_used_len,
    const std::array<std::uint8_t, 6> &reply_src_mac) {
  std::size_t tx_used_len = tx_reply_size_for_rx(rx_used_len);
  if (tx_used_len > tx_buffer_size) {
    throw std::runtime_error(
        "reply packet does not fit in the allocated TX buffer");
  }

  std::memset(tx_packet, 0, tx_buffer_size);

  const std::uint8_t *rx_ethernet = rx_buffer + kVirtioNetHeaderSize;
  std::uint8_t *tx_ethernet = tx_packet + kVirtioNetHeaderSize;
  std::size_t rx_frame_size = rx_used_len - kVirtioNetHeaderSize;
  std::copy(rx_ethernet, rx_ethernet + rx_frame_size, tx_ethernet);

  std::copy(rx_ethernet + 6, rx_ethernet + 12, tx_ethernet);
  std::copy(reply_src_mac.begin(), reply_src_mac.end(), tx_ethernet + 6);
  return tx_used_len;
}

static void print_hex_dump(const std::uint8_t *data, std::size_t size,
                           std::size_t max_bytes) {
  std::size_t count = std::min(size, max_bytes);
  if (count == 0) {
    std::println("  <empty>");
    return;
  }

  for (std::size_t offset = 0; offset < count; offset += 16) {
    std::size_t line_size = std::min<std::size_t>(16, count - offset);
    std::print("  {:04x}: ", offset);
    for (std::size_t i = 0; i < 16; ++i) {
      if (i < line_size)
        std::print("{:02x} ", data[offset + i]);
      else
        std::print("   ");
    }
    std::print(" ");
    for (std::size_t i = 0; i < line_size; ++i) {
      unsigned char ch = data[offset + i];
      std::print("{}", std::isprint(ch) ? static_cast<char>(ch) : '.');
    }
    std::println("");
  }

  if (size > count)
    std::println("  ... {} more bytes not shown", size - count);
}

static void print_ethernet_summary(const std::uint8_t *packet,
                                   std::size_t packet_size) {
  if (packet_size < 14) {
    std::println("  Ethernet frame: <too short: {} bytes>", packet_size);
    return;
  }

  std::uint16_t ethertype = read_be16_from_bytes(packet + 12);
  std::println("  Ethernet dst: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
               packet[0], packet[1], packet[2], packet[3], packet[4],
               packet[5]);
  std::println("  Ethernet src: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
               packet[6], packet[7], packet[8], packet[9], packet[10],
               packet[11]);
  std::println("  Ethernet ethertype: 0x{:04x}", ethertype);
}

static void print_virtio_net_rx_buffer(const std::uint8_t *data,
                                       std::size_t used_len,
                                       std::size_t dump_bytes) {
  if (used_len < kVirtioNetHeaderSize) {
    std::println("  virtio-net header: <too short: {} bytes>", used_len);
    std::println("  Raw buffer prefix:");
    print_hex_dump(data, used_len, dump_bytes);
    return;
  }

  std::println("  virtio-net header:");
  std::println("    flags: 0x{:02x}", data[0]);
  std::println("    gso_type: 0x{:02x}", data[1]);
  std::println("    hdr_len: {}", read_le16_from_bytes(data + 2));
  std::println("    gso_size: {}", read_le16_from_bytes(data + 4));
  std::println("    csum_start: {}", read_le16_from_bytes(data + 6));
  std::println("    csum_offset: {}", read_le16_from_bytes(data + 8));
  std::println("    num_buffers: {}", read_le16_from_bytes(data + 10));

  const std::uint8_t *packet = data + kVirtioNetHeaderSize;
  std::size_t packet_size = used_len - kVirtioNetHeaderSize;
  std::println("  Packet bytes after virtio-net header: {}", packet_size);
  print_ethernet_summary(packet, packet_size);
  std::println("  Raw buffer prefix:");
  print_hex_dump(data, used_len, dump_bytes);
}

static void print_rx_used_entries(std::uint8_t *rx_queue_base,
                                  const VringLayout &rx_layout,
                                  std::uint16_t old_idx,
                                  std::uint16_t new_idx) {
  std::uint16_t count = used_delta(old_idx, new_idx);
  if (count == 0) {
    std::println("RX used entries: <none>");
    return;
  }

  const volatile VringUsedElem *elems =
      vring_used_elems(rx_queue_base, rx_layout);
  std::uint16_t to_print = std::min<std::uint16_t>(count, 8);
  std::println("RX used entries (showing {} of {}):", to_print, count);
  for (std::uint16_t i = 0; i < to_print; ++i) {
    std::uint16_t slot =
        static_cast<std::uint16_t>((old_idx + i) % rx_layout.queue_size);
    std::println("  slot {}: id={} len={}", slot,
                 static_cast<std::uint32_t>(elems[slot].id),
                 static_cast<std::uint32_t>(elems[slot].len));
  }
}

static void print_used_entries(std::string_view label, std::uint8_t *queue_base,
                               const VringLayout &layout,
                               std::uint16_t old_idx,
                               std::uint16_t new_idx) {
  std::uint16_t count = used_delta(old_idx, new_idx);
  if (count == 0) {
    std::println("{} used entries: <none>", label);
    return;
  }

  const volatile VringUsedElem *elems = vring_used_elems(queue_base, layout);
  std::uint16_t to_print = std::min<std::uint16_t>(count, 8);
  std::println("{} used entries (showing {} of {}):", label, to_print, count);
  for (std::uint16_t i = 0; i < to_print; ++i) {
    std::uint16_t slot =
        static_cast<std::uint16_t>((old_idx + i) % layout.queue_size);
    std::println("  slot {}: id={} len={}", slot,
                 static_cast<std::uint32_t>(elems[slot].id),
                 static_cast<std::uint32_t>(elems[slot].len));
  }
}

struct ReceivedPacket {
  std::uint16_t used_slot = 0;
  std::uint32_t desc_id = 0;
  std::uint32_t used_len = 0;
  std::uint16_t ethertype = 0;
  std::uint8_t *buffer = nullptr;
};

static std::optional<ReceivedPacket> wait_for_matching_rx_packet(
    std::uint8_t *dma_base, std::uint8_t *rx_queue_base,
    const DmaLayout &layout, std::uint16_t base_idx,
    std::uint16_t rx_buffers, std::size_t rx_buffer_size,
    std::optional<std::uint16_t> match_ethertype, std::uint32_t wait_ms) {
  std::uint16_t scan_idx = base_idx;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

  for (;;) {
    std::uint16_t current = read_used_idx(rx_queue_base, layout.rx_vring);
    std::uint16_t count = used_delta(scan_idx, current);
    const volatile VringUsedElem *elems =
        vring_used_elems(rx_queue_base, layout.rx_vring);

    for (std::uint16_t i = 0; i < count; ++i) {
      std::uint16_t slot =
          static_cast<std::uint16_t>((scan_idx + i) %
                                     layout.rx_vring.queue_size);
      std::uint32_t id = static_cast<std::uint32_t>(elems[slot].id);
      std::uint32_t len = static_cast<std::uint32_t>(elems[slot].len);

      if (id >= rx_buffers) {
        std::println("Skipping RX used slot {}: descriptor id {} is outside "
                     "posted RX buffers",
                     slot, id);
        continue;
      }
      if (len > rx_buffer_size) {
        std::println("Skipping RX used slot {}: len {} exceeds RX buffer "
                     "size {}",
                     slot, len, rx_buffer_size);
        continue;
      }
      if (len < kVirtioNetHeaderSize + kEthernetHeaderSize) {
        std::println("Skipping RX used slot {}: len {} is too short for "
                     "virtio-net plus Ethernet",
                     slot, len);
        continue;
      }

      std::uint8_t *packet_buffer =
          dma_base + layout.rx_buffers_offset + id * rx_buffer_size;
      const std::uint8_t *ethernet = packet_buffer + kVirtioNetHeaderSize;
      std::uint16_t ethertype = read_be16_from_bytes(ethernet + 12);
      if (match_ethertype && ethertype != *match_ethertype) {
        std::println("Skipping RX used slot {}: ethertype 0x{:04x} does not "
                     "match 0x{:04x}",
                     slot, ethertype, *match_ethertype);
        continue;
      }

      return ReceivedPacket{.used_slot = slot,
                            .desc_id = id,
                            .used_len = len,
                            .ethertype = ethertype,
                            .buffer = packet_buffer};
    }

    scan_idx = current;
    if (wait_ms == 0 || std::chrono::steady_clock::now() >= deadline)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return std::nullopt;
}

static void dry_run(const std::string &bdf, const fs::path &dev_dir,
                    const Options &options) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);
  std::println("RX queue: {}", options.rx_queue);
  std::println("TX queue: {}", options.tx_queue);
  std::println("base IOVA: 0x{:x}", options.iova);
  std::println("vring alignment: {}", options.align);
  std::println("RX buffers: {} x {} bytes", options.rx_buffers,
               options.rx_buffer_size);
  std::println("wait-ms: {}", options.wait_ms);
  std::println("reply src-mac: {}",
               options.src_mac ? mac_string(*options.src_mac)
                               : "virtio-net config MAC");
  std::println("match ethertype: {}",
               options.match_ethertype
                   ? std::format("0x{:04x}", *options.match_ethertype)
                   : std::string("any"));
  std::println("dump-bytes: {}", options.dump_bytes);

  if (options.queue_size) {
    DmaLayout layout = compute_dma_layout(*options.queue_size,
                                          *options.queue_size, options.align,
                                          options.rx_buffers,
                                          options.rx_buffer_size,
                                          options.rx_buffer_size);
    print_dma_layout(layout, options.iova, options.rx_buffers,
                     options.rx_buffer_size);
  } else {
    std::println("queue_size: each target queue's device-reported size");
  }

  std::println("Would open VFIO container/group/device");
  std::println("Would enable PCI Memory Space and Bus Master in CONFIG if "
               "needed");
  std::println("Would mmap virtio COMMON_CFG writable");
  std::println("Would mmap virtio DEVICE_CFG read-only");
  std::println("Would reset device and negotiate FEATURES_OK with "
               "VERSION_1/ACCESS_PLATFORM plus MAC/STATUS when offered");
  std::println("Would print virtio-net config MAC/status");
  std::println("Would inspect RX queue {} and TX queue {}", options.rx_queue,
               options.tx_queue);
  std::println("Would allocate and VFIO-map one DMA area for both vrings and "
               "RX/TX packet buffers");
  std::println("Would publish writable RX descriptors and set RX avail.idx");
  std::println("Would write queue_size, queue_desc, queue_avail, queue_used "
               "for both queues");
  std::println("Would enable both queues");
  std::println("Would write DRIVER_OK, notify RX, then wait up to {} ms for "
               "a matching RX packet",
               options.wait_ms);
  std::println("Would build a TX reply by swapping Ethernet source/destination");
  std::println("Would publish one TX descriptor, notify TX, and poll TX "
               "used.idx for completion");
  std::println("Would restore queue_select, reset the device, then unmap DMA");
  std::println("Would not recycle descriptors or keep the link running.");
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

static void set_driver_ok(MappedRegion &mapping, std::size_t mapping_delta) {
  std::uint8_t status = read_status(mapping, mapping_delta);
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  write_status(mapping, mapping_delta, status);
}

static QueueView program_and_enable_queue(MappedRegion &mapping,
                                          std::size_t mapping_delta,
                                          QueueSelectionGuard &selection,
                                          std::uint16_t queue,
                                          const VringLayout &layout,
                                          std::uint64_t queue_iova,
                                          std::string_view label) {
  selection.select(queue);
  QueueView before = read_queue_view(mapping, mapping_delta);
  print_queue_view(std::string(label) + " before programming", before);
  if (before.enable != 0) {
    throw std::runtime_error(std::string(label) +
                             " is already enabled; reset the device before "
                             "using this tutorial sample");
  }

  program_queue_addresses(mapping, mapping_delta, layout, queue_iova);
  std::println("Wrote {} queue_size, queue_desc, queue_avail, queue_used",
               label);

  QueueView after_program = read_queue_view(mapping, mapping_delta);
  print_queue_view(std::string(label) + " after programming", after_program);

  enable_queue(mapping, mapping_delta);
  std::println("Wrote {} queue_enable=1", label);

  QueueView after_enable = read_queue_view(mapping, mapping_delta);
  print_queue_view(std::string(label) + " after enable", after_enable);
  if (after_enable.enable != 1) {
    throw std::runtime_error(std::string(label) +
                             " queue_enable did not read back as 1");
  }
  return after_enable;
}

static void run_rx_tx_echo(const std::string &bdf, const fs::path &dev_dir,
                           const Options &options) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);
  std::println("RX queue: {}", options.rx_queue);
  std::println("TX queue: {}", options.tx_queue);
  std::println("base IOVA: 0x{:x}", options.iova);
  std::println("vring alignment: {}", options.align);
  std::println("RX buffers: {} x {} bytes", options.rx_buffers,
               options.rx_buffer_size);
  std::println("wait-ms: {}", options.wait_ms);
  std::println("reply src-mac: {}",
               options.src_mac ? mac_string(*options.src_mac)
                               : "virtio-net config MAC");
  std::println("match ethertype: {}",
               options.match_ethertype
                   ? std::format("0x{:04x}", *options.match_ethertype)
                   : std::string("any"));
  std::println("dump-bytes: {}", options.dump_bytes);

  std::size_t page_size = static_cast<std::size_t>(system_page_size());
  if (options.iova % page_size != 0) {
    throw std::runtime_error("base IOVA must be aligned to the system page "
                             "size " +
                             std::to_string(page_size));
  }

  VfioContext vfio = open_vfio_context(bdf, dev_dir);
  print_iommu_info(vfio.container.get());

  ConfigRegion config = get_config_region(vfio.device.get());
  PciCommandGuard pci_command(vfio.device.get(), config);
  pci_command.enable_bus_mastering();

  std::vector<VirtioCap> caps = read_virtio_caps(vfio.device.get(), config);
  VirtioCap common = find_common_cfg(caps);
  VirtioCap notify = find_notify_cfg(caps);
  VirtioCap device_cfg = find_device_cfg(caps);

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
  std::size_t device_cfg_delta = 0;
  MappedRegion device_cfg_mapping =
      map_device_cfg(vfio.device.get(), device_cfg, device_cfg_delta);

  print_status("initial device_status",
               read_status(common_mapping, mapping_delta));
  std::vector<std::uint32_t> guest_features =
      negotiate_minimal_features(common_mapping, mapping_delta);

  VirtioNetConfigView net_config = read_virtio_net_config(
      device_cfg_mapping, device_cfg_delta, device_cfg.length);
  print_virtio_net_config("Virtio-net device config", net_config,
                          guest_features);

  std::uint16_t num_queues =
      common_mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_NUMQ);
  std::uint16_t original_select =
      common_mapping.read16(mapping_delta + VIRTIO_PCI_COMMON_Q_SELECT);
  std::println("num_queues: {}", num_queues);
  std::println("original queue_select: {}", original_select);

  if (options.rx_queue >= num_queues)
    throw std::runtime_error("RX queue index is outside num_queues");
  if (options.tx_queue >= num_queues)
    throw std::runtime_error("TX queue index is outside num_queues");
  if (options.rx_queue == options.tx_queue)
    throw std::runtime_error("RX queue and TX queue must be different");

  QueueSelectionGuard selection(common_mapping, mapping_delta, original_select);

  selection.select(options.rx_queue);
  QueueView rx_before = read_queue_view(common_mapping, mapping_delta);
  print_queue_view("RX queue before sizing", rx_before);
  if (rx_before.enable != 0) {
    throw std::runtime_error(
        "RX queue is already enabled; reset the device before using this "
        "tutorial sample");
  }
  std::uint16_t rx_queue_size =
      choose_queue_size(rx_before.size, options.queue_size);
  if (rx_queue_size != rx_before.size) {
    std::println("using reduced RX queue_size: {} (device max {})",
                 rx_queue_size, rx_before.size);
  }

  selection.select(options.tx_queue);
  QueueView tx_before = read_queue_view(common_mapping, mapping_delta);
  print_queue_view("TX queue before sizing", tx_before);
  if (tx_before.enable != 0) {
    throw std::runtime_error(
        "TX queue is already enabled; reset the device before using this "
        "tutorial sample");
  }
  std::uint16_t tx_queue_size =
      choose_queue_size(tx_before.size, options.queue_size);
  if (tx_queue_size != tx_before.size) {
    std::println("using reduced TX queue_size: {} (device max {})",
                 tx_queue_size, tx_before.size);
  }

  std::array<std::uint8_t, 6> reply_src_mac =
      options.src_mac.value_or(net_config.mac);
  std::size_t tx_buffer_size = options.rx_buffer_size;
  std::println("Reply frame policy:");
  std::println("  destination: received Ethernet source");
  std::println("  source: {}", mac_string(reply_src_mac));
  std::println("  ethertype/payload: copied from received frame");
  std::println("  TX scratch buffer bytes: {}", tx_buffer_size);

  DmaLayout layout = compute_dma_layout(rx_queue_size, tx_queue_size,
                                        options.align, options.rx_buffers,
                                        options.rx_buffer_size,
                                        tx_buffer_size);
  print_dma_layout(layout, options.iova, options.rx_buffers,
                   options.rx_buffer_size);

  AnonymousBuffer buffer(layout.total_size);
  buffer.zero();

  constexpr std::uint32_t dma_flags =
      VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
  std::println("DMA permissions: {}", dma_flag_names(dma_flags));

  DmaMapping dma(vfio.container.get(), buffer.data(), options.iova,
                 layout.total_size, dma_flags);
  std::println("Mapped zeroed DMA buffer {:p} to IOVA 0x{:x} ({} bytes)",
               static_cast<void *>(buffer.data()), dma.iova(), dma.size());

  DeviceResetGuard reset_guard(common_mapping, mapping_delta);

  std::uint8_t *rx_queue_base = buffer.data() + layout.rx_vring_offset;
  std::uint8_t *tx_queue_base = buffer.data() + layout.tx_vring_offset;
  std::uint8_t *tx_packet = buffer.data() + layout.tx_packet_offset;
  std::uint64_t rx_queue_iova = options.iova + layout.rx_vring_offset;
  std::uint64_t tx_queue_iova = options.iova + layout.tx_vring_offset;
  std::uint64_t rx_buffer_iova = options.iova + layout.rx_buffers_offset;
  std::uint64_t tx_packet_iova = options.iova + layout.tx_packet_offset;

  publish_rx_buffers(rx_queue_base, layout.rx_vring, rx_buffer_iova,
                     options.rx_buffers, options.rx_buffer_size);
  std::println("Published {} writable RX descriptors", options.rx_buffers);
  std::println("RX avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(rx_queue_base, layout.rx_vring)));
  std::println("Initial RX used.idx: {}",
               read_used_idx(rx_queue_base, layout.rx_vring));

  std::println("Initial TX used.idx: {}",
               read_used_idx(tx_queue_base, layout.tx_vring));

  QueueView rx_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.rx_queue,
      layout.rx_vring, rx_queue_iova, "RX queue");
  QueueView tx_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.tx_queue,
      layout.tx_vring, tx_queue_iova, "TX queue");

  std::size_t rx_notify_delta = 0;
  std::uint64_t rx_notify_bar_offset = 0;
  off_t rx_notify_mmap_offset = 0;
  MappedRegion rx_notify_mapping = map_notify_window(
      vfio.device.get(), notify, notify_region, rx_after_enable.notify_off,
      rx_notify_delta, rx_notify_bar_offset, rx_notify_mmap_offset);
  std::println("RX notify write target:");
  std::println("  BAR{} relative offset: 0x{:x}", notify.bar,
               rx_notify_bar_offset);
  std::println("  VFIO device-fd file offset: 0x{:x}",
               rx_notify_mmap_offset + static_cast<off_t>(rx_notify_delta));
  std::println("  mmap file offset: 0x{:x}", rx_notify_mmap_offset);
  std::println("  mmap delta: 0x{:x}", rx_notify_delta);

  std::size_t tx_notify_delta = 0;
  std::uint64_t tx_notify_bar_offset = 0;
  off_t tx_notify_mmap_offset = 0;
  MappedRegion tx_notify_mapping = map_notify_window(
      vfio.device.get(), notify, notify_region, tx_after_enable.notify_off,
      tx_notify_delta, tx_notify_bar_offset, tx_notify_mmap_offset);
  std::println("TX notify write target:");
  std::println("  BAR{} relative offset: 0x{:x}", notify.bar,
               tx_notify_bar_offset);
  std::println("  VFIO device-fd file offset: 0x{:x}",
               tx_notify_mmap_offset + static_cast<off_t>(tx_notify_delta));
  std::println("  mmap file offset: 0x{:x}", tx_notify_mmap_offset);
  std::println("  mmap delta: 0x{:x}", tx_notify_delta);

  std::uint16_t rx_notify_value = rx_after_enable.selected;
  if (rx_after_enable.notify_data) {
    std::println("  RX queue_notify_data candidate: {}",
                 *rx_after_enable.notify_data);
  }
  std::uint16_t tx_notify_value = tx_after_enable.selected;
  if (tx_after_enable.notify_data) {
    std::println("  TX queue_notify_data candidate: {}",
                 *tx_after_enable.notify_data);
  }
  std::println("  RX notify value: {} (queue index; "
               "VIRTIO_F_NOTIFICATION_DATA was not negotiated)",
               rx_notify_value);
  std::println("  TX notify value: {} (queue index; "
               "VIRTIO_F_NOTIFICATION_DATA was not negotiated)",
               tx_notify_value);

  std::uint16_t rx_used_base_idx = read_used_idx(rx_queue_base, layout.rx_vring);
  std::atomic_thread_fence(std::memory_order_release);
  set_driver_ok(common_mapping, mapping_delta);
  print_status("after DRIVER_OK", read_status(common_mapping, mapping_delta));

  write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
  std::println("Wrote one 16-bit RX notify value");
  std::println("Waiting up to {} ms for a matching RX packet",
               options.wait_ms);

  std::optional<ReceivedPacket> received = wait_for_matching_rx_packet(
      buffer.data(), rx_queue_base, layout, rx_used_base_idx,
      options.rx_buffers, options.rx_buffer_size, options.match_ethertype,
      options.wait_ms);

  std::uint16_t rx_used_after_wait =
      read_used_idx(rx_queue_base, layout.rx_vring);
  std::println("RX used.idx after wait: {} (delta {})", rx_used_after_wait,
               used_delta(rx_used_base_idx, rx_used_after_wait));
  print_rx_used_entries(rx_queue_base, layout.rx_vring, rx_used_base_idx,
                        rx_used_after_wait);

  if (!received) {
    std::string match_text =
        options.match_ethertype
            ? std::format("0x{:04x}", *options.match_ethertype)
            : std::string("any");
    throw std::runtime_error("no RX packet matched ethertype " + match_text);
  }

  const std::uint8_t *rx_ethernet =
      received->buffer + kVirtioNetHeaderSize;
  std::array<std::uint8_t, 6> reply_dst_mac =
      mac_from_bytes(rx_ethernet + 6);
  std::println("Selected RX packet for reply:");
  std::println("  used slot: {}", received->used_slot);
  std::println("  descriptor id: {}", received->desc_id);
  std::println("  used len: {}", received->used_len);
  std::println("  ethertype: 0x{:04x}", received->ethertype);
  std::println("  reply dst-mac: {}", mac_string(reply_dst_mac));
  std::println("  reply src-mac: {}", mac_string(reply_src_mac));
  std::println("Selected RX buffer dump:");
  print_virtio_net_rx_buffer(received->buffer, received->used_len,
                             options.dump_bytes);

  std::uint16_t tx_used_base_idx = read_used_idx(tx_queue_base, layout.tx_vring);
  std::size_t tx_used_len =
      build_tx_reply_from_rx(tx_packet, layout.tx_packet_size,
                             received->buffer, received->used_len,
                             reply_src_mac);
  publish_tx_packet(tx_queue_base, layout.tx_vring, tx_packet_iova,
                    tx_used_len);
  std::println("Published one TX reply descriptor");
  std::println("TX avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(tx_queue_base, layout.tx_vring)));
  std::println("TX reply packet bytes: {}", tx_used_len);
  std::println("TX reply packet prefix:");
  print_hex_dump(tx_packet, tx_used_len, options.dump_bytes);

  write_notify(tx_notify_mapping, tx_notify_delta, tx_notify_value);
  std::println("Wrote one 16-bit TX notify value");
  std::println("Waiting up to {} ms for TX completion", options.wait_ms);

  if (options.wait_ms > 0) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::uint16_t current = read_used_idx(tx_queue_base, layout.tx_vring);
      if (used_delta(tx_used_base_idx, current) >= 1)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::uint16_t tx_used_after_wait =
      read_used_idx(tx_queue_base, layout.tx_vring);
  std::println("TX used.idx after wait: {} (delta {})", tx_used_after_wait,
               used_delta(tx_used_base_idx, tx_used_after_wait));
  print_used_entries("TX", tx_queue_base, layout.tx_vring, tx_used_base_idx,
                     tx_used_after_wait);
  print_status("after wait", read_status(common_mapping, mapping_delta));

  selection.restore();
  std::println("Restored queue_select to {}", original_select);

  reset_device(common_mapping, mapping_delta, "after cleanup reset");
  reset_guard.disarm();
  pci_command.restore();

  std::uint64_t unmapped = dma.unmap();
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped",
               options.iova, unmapped);
  std::println("Did not recycle descriptors or keep the link running.");
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
      if (arg == "--rx-queue" && i + 1 < argc) {
        options.rx_queue = parse_u16(argv[++i], "rx-queue");
      } else if (arg == "--tx-queue" && i + 1 < argc) {
        options.tx_queue = parse_u16(argv[++i], "tx-queue");
      } else if (arg == "--queue-size" && i + 1 < argc) {
        options.queue_size = parse_u16(argv[++i], "queue-size");
      } else if (arg == "--rx-buffers" && i + 1 < argc) {
        options.rx_buffers = parse_u16(argv[++i], "rx-buffers");
      } else if (arg == "--rx-buffer-size" && i + 1 < argc) {
        options.rx_buffer_size =
            static_cast<std::size_t>(parse_ull(argv[++i], "rx-buffer-size"));
      } else if (arg == "--iova" && i + 1 < argc) {
        options.iova = parse_ull(argv[++i], "iova");
      } else if (arg == "--align" && i + 1 < argc) {
        options.align = static_cast<std::size_t>(parse_ull(argv[++i], "align"));
      } else if (arg == "--wait-ms" && i + 1 < argc) {
        options.wait_ms = parse_u32(argv[++i], "wait-ms");
      } else if (arg == "--src-mac" && i + 1 < argc) {
        options.src_mac = parse_mac(argv[++i], "src-mac");
      } else if (arg == "--match-ethertype" && i + 1 < argc) {
        options.match_ethertype = parse_u16(argv[++i], "match-ethertype");
      } else if (arg == "--accept-any-ethertype") {
        options.match_ethertype.reset();
      } else if (arg == "--dump-bytes" && i + 1 < argc) {
        options.dump_bytes =
            static_cast<std::size_t>(parse_ull(argv[++i], "dump-bytes"));
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
      run_rx_tx_echo(bdf, dev_dir, options);
    else
      dry_run(bdf, dev_dir, options);

    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
