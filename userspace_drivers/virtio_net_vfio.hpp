#pragma once

#include "vfio_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
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
#include <thread>
#include <vector>

static_assert(std::endian::native == std::endian::little,
              "these tutorial samples expect a little-endian host");

constexpr std::size_t kFeatureWords = 3;
constexpr std::size_t kVringDescSize = 16;
constexpr std::size_t kVringUsedElemSize = 8;
constexpr std::size_t kVringUsedAlignSize = 4;
constexpr std::uint16_t kDefaultRxQueue = 0;
constexpr std::uint16_t kDefaultTxQueue = 1;
constexpr std::uint16_t kDefaultRxBuffers = 64;
constexpr std::uint16_t kDefaultTxBuffers = 8;
constexpr std::size_t kDefaultRxBufferSize = 2048;
constexpr std::size_t kDefaultDumpBytes = 160;
constexpr std::size_t kVirtioNetHeaderSize = 12;
constexpr std::uint16_t kDefaultEthertype = 0x88b5;
constexpr std::size_t kEthernetHeaderSize = 14;
constexpr std::size_t kEthernetMinFrameSize = 60;
constexpr std::uint16_t kVringDescFWrite = 2;

// linux/virtio_net.h contains a C field named "class", so these samples keep
// the few virtio-net feature bit numbers and config offsets they need here.
constexpr unsigned VIRTIO_NET_F_MAC = 5;
constexpr unsigned VIRTIO_NET_F_STATUS = 16;
constexpr std::size_t kVirtioNetConfigMac = 0;
constexpr std::size_t kVirtioNetConfigStatus = 6;
constexpr std::size_t kVirtioNetConfigMaxVirtqueuePairs = 8;
constexpr std::size_t kVirtioNetConfigMtu = 10;
constexpr std::size_t kVirtioNetConfigMinBytes = 12;

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
  std::size_t tx_buffers_offset = 0;
  std::size_t tx_buffers_size = 0;
  std::size_t tx_buffer_size = 0;
  std::size_t total_size = 0;
};

struct ReceivedPacket {
  std::uint16_t used_slot = 0;
  std::uint32_t desc_id = 0;
  std::uint32_t used_len = 0;
  std::uint16_t ethertype = 0;
  std::uint8_t *buffer = nullptr;
};

inline std::string errno_text(int error) { return std::strerror(error); }

inline std::string normalize_bdf(std::string bdf) {
  if (std::count(bdf.begin(), bdf.end(), ':') == 1)
    bdf = "0000:" + bdf;
  return bdf;
}

inline unsigned long long parse_ull(std::string_view text,
                                    std::string_view name) {
  std::size_t parsed = 0;
  std::string owned(text);
  unsigned long long value = std::stoull(owned, &parsed, 0);
  if (parsed != owned.size())
    throw std::runtime_error("invalid " + std::string(name) + ": " + owned);
  return value;
}

inline std::uint16_t parse_u16(std::string_view text, std::string_view name) {
  unsigned long long value = parse_ull(text, name);
  if (value > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }
  return static_cast<std::uint16_t>(value);
}

inline std::array<std::uint8_t, 6> parse_mac(std::string_view text,
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

inline std::uint32_t parse_u32(std::string_view text, std::string_view name) {
  unsigned long long value = parse_ull(text, name);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }
  return static_cast<std::uint32_t>(value);
}

inline bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

inline std::size_t align_up(std::size_t value, std::size_t align) {
  if (!is_power_of_two(align))
    throw std::runtime_error("alignment must be a power of two");
  if (value > std::numeric_limits<std::size_t>::max() - (align - 1))
    throw std::runtime_error("size overflow while aligning value");
  return (value + align - 1) & ~(align - 1);
}

inline long system_page_size() {
  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    throw std::runtime_error("cannot determine system page size");
  return page_size;
}

inline std::size_t round_up_to_page(std::size_t size) {
  return align_up(size, static_cast<std::size_t>(system_page_size()));
}

inline std::size_t checked_add(std::size_t a, std::size_t b,
                               std::string_view what) {
  if (a > std::numeric_limits<std::size_t>::max() - b)
    throw std::runtime_error(std::string(what) + " size overflow");
  return a + b;
}

inline std::size_t checked_mul(std::size_t a, std::size_t b,
                               std::string_view what) {
  if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
    throw std::runtime_error(std::string(what) + " size overflow");
  return a * b;
}

inline std::string join_flags(const std::vector<std::string_view> &flags) {
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

inline std::string status_names(std::uint8_t status) {
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

inline std::string dma_flag_names(std::uint32_t flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_DMA_MAP_FLAG_READ)
    names.push_back("READ");
  if (flags & VFIO_DMA_MAP_FLAG_WRITE)
    names.push_back("WRITE");
  return join_flags(names);
}

inline std::string pci_command_names(std::uint16_t command) {
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

inline std::string mac_string(const std::array<std::uint8_t, 6> &mac) {
  return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0],
                     mac[1], mac[2], mac[3], mac[4], mac[5]);
}

inline void print_status(std::string_view label, std::uint8_t status) {
  std::println("{}: 0x{:02x} ({})", label, status, status_names(status));
}

inline bool feature_is_set(const std::vector<std::uint32_t> &words,
                           unsigned bit) {
  std::size_t word = bit / 32;
  unsigned shift = bit % 32;
  return word < words.size() && (words[word] & (1u << shift));
}

inline void set_feature(std::vector<std::uint32_t> &words, unsigned bit) {
  std::size_t word = bit / 32;
  unsigned shift = bit % 32;
  if (word >= words.size())
    throw std::runtime_error("feature bit is outside the configured word range");
  words[word] |= 1u << shift;
}

inline void print_feature_words(std::string_view label,
                                const std::vector<std::uint32_t> &words) {
  std::println("{}:", label);
  for (std::size_t i = 0; i < words.size(); ++i) {
    std::println("  word {} (bits {}..{}): 0x{:08x}", i, i * 32,
                 i * 32 + 31, words[i]);
  }
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

inline std::uint8_t read_status(MappedRegion &mapping,
                                std::size_t mapping_delta) {
  return mapping.read8(mapping_delta + VIRTIO_PCI_COMMON_STATUS);
}

inline void write_status(MappedRegion &mapping, std::size_t mapping_delta,
                         std::uint8_t status) {
  mapping.write8(mapping_delta + VIRTIO_PCI_COMMON_STATUS, status);
}

inline void wait_for_status(MappedRegion &mapping, std::size_t mapping_delta,
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

inline void reset_device(MappedRegion &mapping, std::size_t mapping_delta,
                         std::string_view label) {
  write_status(mapping, mapping_delta, 0);
  wait_for_status(mapping, mapping_delta, 0, std::chrono::milliseconds(1000));
  print_status(label, read_status(mapping, mapping_delta));
}

inline std::uint64_t read_common_u64(MappedRegion &mapping,
                                     std::size_t mapping_delta,
                                     std::size_t lo_offset,
                                     std::size_t hi_offset) {
  std::uint64_t lo = mapping.read32(mapping_delta + lo_offset);
  std::uint64_t hi = mapping.read32(mapping_delta + hi_offset);
  return lo | (hi << 32);
}

inline void write_common_u64(MappedRegion &mapping, std::size_t mapping_delta,
                             std::size_t lo_offset, std::size_t hi_offset,
                             std::uint64_t value) {
  mapping.write32(mapping_delta + lo_offset,
                  static_cast<std::uint32_t>(value & 0xffffffffu));
  mapping.write32(mapping_delta + hi_offset,
                  static_cast<std::uint32_t>(value >> 32));
}

inline QueueView read_queue_view(MappedRegion &mapping,
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

inline void print_queue_view(std::string_view label, const QueueView &view) {
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

inline VirtioNetConfigView read_virtio_net_config(MappedRegion &mapping,
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

inline void print_virtio_net_config(
    std::string_view label, const VirtioNetConfigView &view,
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

inline FeatureWords read_feature_words(MappedRegion &mapping,
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

inline std::vector<std::uint32_t>
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

inline void write_guest_feature_words(
    MappedRegion &mapping, std::size_t mapping_delta,
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

inline std::vector<std::uint32_t>
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

inline VringLayout compute_vring_layout(std::uint16_t queue_size,
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

inline void print_vring_layout(const VringLayout &layout,
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

inline DmaLayout compute_dma_layout(std::uint16_t rx_queue_size,
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
  layout.tx_buffers_offset = layout.tx_packet_offset;
  layout.tx_buffers_size = tx_packet_size;
  layout.tx_buffer_size = tx_packet_size;
  layout.total_size =
      round_up_to_page(checked_add(layout.tx_packet_offset,
                                   layout.tx_packet_size,
                                   "DMA mapping"));
  return layout;
}

inline DmaLayout compute_dma_layout(std::uint16_t rx_queue_size,
                                    std::uint16_t tx_queue_size,
                                    std::size_t align,
                                    std::uint16_t rx_buffers,
                                    std::uint16_t tx_buffers,
                                    std::size_t rx_buffer_size,
                                    std::size_t tx_buffer_size) {
  if (rx_buffers == 0)
    throw std::runtime_error("rx-buffers must be greater than 0");
  if (rx_buffers > rx_queue_size) {
    throw std::runtime_error("rx-buffers must not exceed RX queue_size " +
                             std::to_string(rx_queue_size));
  }
  if (tx_buffers == 0)
    throw std::runtime_error("tx-buffers must be greater than 0");
  if (tx_buffers > tx_queue_size) {
    throw std::runtime_error("tx-buffers must not exceed TX queue_size " +
                             std::to_string(tx_queue_size));
  }
  if (rx_buffer_size == 0)
    throw std::runtime_error("rx-buffer-size must be greater than 0");
  if (tx_buffer_size == 0)
    throw std::runtime_error("tx-buffer-size must be greater than 0");

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
  layout.tx_buffers_offset =
      round_up_to_page(checked_add(layout.rx_buffers_offset,
                                   layout.rx_buffers_size,
                                   "RX buffer area end"));
  layout.tx_buffer_size = tx_buffer_size;
  layout.tx_buffers_size =
      checked_mul(tx_buffers, tx_buffer_size, "TX buffer area");
  layout.tx_packet_offset = layout.tx_buffers_offset;
  layout.tx_packet_size = tx_buffer_size;
  layout.total_size =
      round_up_to_page(checked_add(layout.tx_buffers_offset,
                                   layout.tx_buffers_size,
                                   "DMA mapping"));
  return layout;
}

inline void print_dma_layout(const DmaLayout &layout, std::uint64_t base_iova,
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

inline void print_dma_layout(const DmaLayout &layout, std::uint64_t base_iova,
                             std::uint16_t rx_buffers,
                             std::uint16_t tx_buffers,
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
  std::println("  TX buffers offset: 0x{:x}", layout.tx_buffers_offset);
  std::println("  TX buffers IOVA: 0x{:x}",
               base_iova + layout.tx_buffers_offset);
  std::println("  TX buffers: {} x {} bytes", tx_buffers,
               layout.tx_buffer_size);
  std::println("  TX buffer bytes: {}", layout.tx_buffers_size);
  std::println("  mapped bytes: {}", layout.total_size);
}

inline std::uint16_t choose_queue_size(
    std::uint16_t device_queue_size,
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

inline VringDesc *vring_descs(std::uint8_t *queue_base,
                              const VringLayout &layout) {
  return reinterpret_cast<VringDesc *>(queue_base + layout.desc_offset);
}

inline std::uint16_t *vring_avail_ring(std::uint8_t *queue_base,
                                       const VringLayout &layout) {
  return reinterpret_cast<std::uint16_t *>(queue_base + layout.avail_offset +
                                           sizeof(std::uint16_t) * 2);
}

inline volatile std::uint16_t *
vring_avail_idx(std::uint8_t *queue_base, const VringLayout &layout) {
  return reinterpret_cast<volatile std::uint16_t *>(
      queue_base + layout.avail_offset + sizeof(std::uint16_t));
}

inline volatile std::uint16_t *
vring_used_idx(std::uint8_t *queue_base, const VringLayout &layout) {
  return reinterpret_cast<volatile std::uint16_t *>(
      queue_base + layout.used_offset + sizeof(std::uint16_t));
}

inline volatile const VringUsedElem *
vring_used_elems(const std::uint8_t *queue_base, const VringLayout &layout) {
  return reinterpret_cast<volatile const VringUsedElem *>(
      queue_base + layout.used_offset + sizeof(std::uint16_t) * 2);
}

inline void publish_rx_buffers(std::uint8_t *rx_queue_base,
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

inline void recycle_rx_buffer(std::uint8_t *rx_queue_base,
                              const VringLayout &rx_layout,
                              std::uint16_t desc_id) {
  std::uint16_t avail_idx = *vring_avail_idx(rx_queue_base, rx_layout);
  std::uint16_t *avail_ring = vring_avail_ring(rx_queue_base, rx_layout);
  avail_ring[avail_idx % rx_layout.queue_size] = desc_id;
  std::atomic_thread_fence(std::memory_order_release);
  *vring_avail_idx(rx_queue_base, rx_layout) =
      static_cast<std::uint16_t>(avail_idx + 1);
  std::atomic_thread_fence(std::memory_order_release);
}

inline void publish_tx_packet(std::uint8_t *tx_queue_base,
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

inline void publish_tx_packet(std::uint8_t *tx_queue_base,
                              const VringLayout &tx_layout,
                              std::uint16_t desc_id,
                              std::uint64_t tx_packet_iova,
                              std::size_t tx_packet_size) {
  if (tx_packet_size > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("TX packet is too large for a vring desc");

  VringDesc *desc = vring_descs(tx_queue_base, tx_layout);
  std::uint16_t *avail_ring = vring_avail_ring(tx_queue_base, tx_layout);
  std::uint16_t avail_idx = *vring_avail_idx(tx_queue_base, tx_layout);
  desc[desc_id].addr = tx_packet_iova;
  desc[desc_id].len = static_cast<std::uint32_t>(tx_packet_size);
  desc[desc_id].flags = 0;
  desc[desc_id].next = 0;
  avail_ring[avail_idx % tx_layout.queue_size] = desc_id;

  std::atomic_thread_fence(std::memory_order_release);
  *vring_avail_idx(tx_queue_base, tx_layout) =
      static_cast<std::uint16_t>(avail_idx + 1);
  std::atomic_thread_fence(std::memory_order_release);
}

inline std::uint16_t read_used_idx(std::uint8_t *queue_base,
                                   const VringLayout &layout) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return *vring_used_idx(queue_base, layout);
}

inline std::uint16_t used_delta(std::uint16_t before, std::uint16_t after) {
  return static_cast<std::uint16_t>(after - before);
}

inline std::uint16_t read_le16_from_bytes(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

inline std::uint16_t read_be16_from_bytes(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(bytes[1]) |
         (static_cast<std::uint16_t>(bytes[0]) << 8);
}

inline std::array<std::uint8_t, 6> mac_from_bytes(const std::uint8_t *bytes) {
  std::array<std::uint8_t, 6> mac{};
  std::copy(bytes, bytes + mac.size(), mac.begin());
  return mac;
}

inline std::size_t tx_reply_size_for_rx(std::size_t rx_used_len) {
  if (rx_used_len < kVirtioNetHeaderSize + kEthernetHeaderSize) {
    throw std::runtime_error(
        "received buffer is too short for virtio-net header plus Ethernet");
  }

  std::size_t rx_frame_size = rx_used_len - kVirtioNetHeaderSize;
  return kVirtioNetHeaderSize +
         std::max(rx_frame_size, kEthernetMinFrameSize);
}

inline std::size_t build_tx_reply_from_rx(
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

inline void print_hex_dump(const std::uint8_t *data, std::size_t size,
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

inline void print_ethernet_summary(const std::uint8_t *packet,
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

inline void print_virtio_net_rx_buffer(const std::uint8_t *data,
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

inline void print_rx_used_entries(std::uint8_t *rx_queue_base,
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

inline void print_used_entries(std::string_view label, std::uint8_t *queue_base,
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

inline std::optional<ReceivedPacket> read_rx_used_packet(
    std::uint8_t *dma_base, std::uint8_t *rx_queue_base,
    const DmaLayout &layout, std::uint16_t used_slot,
    std::uint16_t rx_buffers, std::size_t rx_buffer_size,
    std::optional<std::uint16_t> match_ethertype) {
  const volatile VringUsedElem *elems =
      vring_used_elems(rx_queue_base, layout.rx_vring);
  std::uint32_t id = static_cast<std::uint32_t>(elems[used_slot].id);
  std::uint32_t len = static_cast<std::uint32_t>(elems[used_slot].len);

  if (id >= rx_buffers) {
    std::println("Skipping RX used slot {}: descriptor id {} is outside "
                 "posted RX buffers",
                 used_slot, id);
    return std::nullopt;
  }
  if (len > rx_buffer_size) {
    std::println("Skipping RX used slot {}: len {} exceeds RX buffer size {}",
                 used_slot, len, rx_buffer_size);
    return std::nullopt;
  }
  if (len < kVirtioNetHeaderSize + kEthernetHeaderSize) {
    std::println("Skipping RX used slot {}: len {} is too short for "
                 "virtio-net plus Ethernet",
                 used_slot, len);
    return std::nullopt;
  }

  std::uint8_t *packet_buffer =
      dma_base + layout.rx_buffers_offset + id * rx_buffer_size;
  const std::uint8_t *ethernet = packet_buffer + kVirtioNetHeaderSize;
  std::uint16_t ethertype = read_be16_from_bytes(ethernet + 12);
  if (match_ethertype && ethertype != *match_ethertype) {
    std::println("Skipping RX used slot {}: ethertype 0x{:04x} does not "
                 "match 0x{:04x}",
                 used_slot, ethertype, *match_ethertype);
    return std::nullopt;
  }

  return ReceivedPacket{.used_slot = used_slot,
                        .desc_id = id,
                        .used_len = len,
                        .ethertype = ethertype,
                        .buffer = packet_buffer};
}

inline std::optional<ReceivedPacket> wait_for_matching_rx_packet(
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

    for (std::uint16_t i = 0; i < count; ++i) {
      std::uint16_t slot =
          static_cast<std::uint16_t>((scan_idx + i) %
                                     layout.rx_vring.queue_size);
      std::optional<ReceivedPacket> received = read_rx_used_packet(
          dma_base, rx_queue_base, layout, slot, rx_buffers, rx_buffer_size,
          match_ethertype);
      if (received)
        return received;
    }

    scan_idx = current;
    if (wait_ms == 0 || std::chrono::steady_clock::now() >= deadline)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return std::nullopt;
}

inline void program_queue_addresses(MappedRegion &mapping,
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

inline void enable_queue(MappedRegion &mapping, std::size_t mapping_delta) {
  mapping.write16(mapping_delta + VIRTIO_PCI_COMMON_Q_ENABLE, 1);
}

inline void write_notify(MappedRegion &mapping, std::size_t mapping_delta,
                         std::uint16_t value) {
  mapping.write16(mapping_delta, value);
}

inline void set_driver_ok(MappedRegion &mapping, std::size_t mapping_delta) {
  std::uint8_t status = read_status(mapping, mapping_delta);
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  write_status(mapping, mapping_delta, status);
}

inline QueueView program_and_enable_queue(MappedRegion &mapping,
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
