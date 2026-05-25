// Exercise 27: Send a virtio-net control virtqueue command through VFIO
//
// Goal: configure RX queue 0, TX queue 1, and the virtio-net control queue,
// then send VIRTIO_NET_CTRL_RX_PROMISC through the control virtqueue and read
// the device's ACK byte.
//
// Background
// ----------
// The previous virtio-net samples used only queue 0 (RX) and queue 1 (TX).
// This device reports num_queues=3 because it also offers
// VIRTIO_NET_F_CTRL_VQ: queue 2 is the control virtqueue.
//
// Control virtqueue commands are descriptor chains:
//
//   readable control header -> readable command data -> writable ACK byte
//
// This sample negotiates VIRTIO_NET_F_CTRL_VQ and VIRTIO_NET_F_CTRL_RX, sends
// the RX promiscuous-mode command, waits for the control used ring, prints the
// ACK, then resets the device so the mode change is not left behind.
//
// New concepts
// ------------
// - Negotiating virtio-net control virtqueue features
// - Programming the third queue as the control virtqueue
// - Publishing a descriptor chain with NEXT and WRITE flags
// - Reading the control ACK byte written by the device

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <linux/vfio.h>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "virtio_net_vfio.hpp"

namespace fs = std::filesystem;

constexpr std::uint8_t kVirtioNetCtrlRx = 0;
constexpr std::uint8_t kVirtioNetCtrlRxPromisc = 0;
constexpr std::uint8_t kVirtioNetOk = 0;
constexpr std::uint8_t kVirtioNetErr = 1;
constexpr std::size_t kCtrlHdrSize = 2;
constexpr std::size_t kCtrlStateSize = 1;
constexpr std::size_t kCtrlAckSize = 1;
constexpr std::uint8_t kCtrlAckInitial = 0xff;

struct Options {
  std::uint16_t rx_queue = kDefaultRxQueue;
  std::uint16_t tx_queue = kDefaultTxQueue;
  std::uint16_t ctrl_queue = 2;
  std::optional<std::uint16_t> queue_size;
  std::uint64_t iova = 0x100000000ull;
  std::size_t align = 4096;
  std::uint16_t rx_buffers = kDefaultRxBuffers;
  std::size_t rx_buffer_size = kDefaultRxBufferSize;
  std::uint32_t wait_ms = 5000;
  bool promisc = true;
  bool yes = false;
};

struct ControlDmaLayout {
  VringLayout rx_vring;
  VringLayout tx_vring;
  VringLayout ctrl_vring;
  std::size_t rx_vring_offset = 0;
  std::size_t tx_vring_offset = 0;
  std::size_t ctrl_vring_offset = 0;
  std::size_t rx_buffers_offset = 0;
  std::size_t rx_buffers_size = 0;
  std::size_t ctrl_command_offset = 0;
  std::size_t ctrl_hdr_offset = 0;
  std::size_t ctrl_state_offset = 0;
  std::size_t ctrl_ack_offset = 0;
  std::size_t total_size = 0;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} <BDF> [--rx-queue <n>] [--tx-queue <n>] [--ctrl-queue <n>]\n"
      "       [--queue-size <n>] [--rx-buffers <n>]\n"
      "       [--rx-buffer-size <bytes>] [--iova <addr>]\n"
      "       [--align <bytes>] [--wait-ms <n>]\n"
      "       [--promisc on|off] [--dry-run|--yes]\n\n"
      "Defaults:\n"
      "  rx-queue = 0\n"
      "  tx-queue = 1\n"
      "  ctrl-queue = 2\n"
      "  queue-size = each target queue's device-reported size\n"
      "  rx-buffers = 64\n"
      "  rx-buffer-size = 2048\n"
      "  iova = 0x100000000\n"
      "  align = 4096\n"
      "  wait-ms = 5000\n"
      "  promisc = on\n"
      "  default mode = dry-run\n\n"
      "Examples:\n"
      "  {} c1:00.6\n"
      "  {} c1:00.6 --promisc on --yes\n"
      "  {} c1:00.6 --queue-size 128 --promisc off --yes",
      argv0, argv0, argv0, argv0);
}

static std::string on_off(bool value) { return value ? "on" : "off"; }

static bool parse_on_off(std::string_view text, std::string_view name) {
  if (text == "on" || text == "1" || text == "true" || text == "yes")
    return true;
  if (text == "off" || text == "0" || text == "false" || text == "no")
    return false;
  throw std::runtime_error("invalid " + std::string(name) + ": " +
                           std::string(text));
}

static std::string ctrl_ack_name(std::uint8_t ack) {
  if (ack == kVirtioNetOk)
    return "OK";
  if (ack == kVirtioNetErr)
    return "ERR";
  if (ack == kCtrlAckInitial)
    return "not-written";
  return "unknown";
}

static ControlDmaLayout
compute_control_dma_layout(std::uint16_t rx_queue_size,
                           std::uint16_t tx_queue_size,
                           std::uint16_t ctrl_queue_size, std::size_t align,
                           std::uint16_t rx_buffers,
                           std::size_t rx_buffer_size) {
  if (rx_buffers == 0)
    throw std::runtime_error("rx-buffers must be greater than 0");
  if (rx_buffers > rx_queue_size) {
    throw std::runtime_error("rx-buffers must not exceed RX queue_size " +
                             std::to_string(rx_queue_size));
  }
  if (rx_buffer_size == 0)
    throw std::runtime_error("rx-buffer-size must be greater than 0");
  if (ctrl_queue_size < 3) {
    throw std::runtime_error(
        "control queue-size must be at least 3 for this descriptor chain");
  }

  ControlDmaLayout layout;
  layout.rx_vring = compute_vring_layout(rx_queue_size, align);
  layout.tx_vring = compute_vring_layout(tx_queue_size, align);
  layout.ctrl_vring = compute_vring_layout(ctrl_queue_size, align);
  layout.rx_vring_offset = 0;
  layout.tx_vring_offset = round_up_to_page(layout.rx_vring.total_size);
  layout.ctrl_vring_offset =
      round_up_to_page(checked_add(layout.tx_vring_offset,
                                   layout.tx_vring.total_size,
                                   "TX vring end"));
  layout.rx_buffers_offset =
      round_up_to_page(checked_add(layout.ctrl_vring_offset,
                                   layout.ctrl_vring.total_size,
                                   "control vring end"));
  layout.rx_buffers_size =
      checked_mul(rx_buffers, rx_buffer_size, "RX buffer area");
  layout.ctrl_command_offset =
      round_up_to_page(checked_add(layout.rx_buffers_offset,
                                   layout.rx_buffers_size,
                                   "RX buffer area end"));
  layout.ctrl_hdr_offset = layout.ctrl_command_offset;
  layout.ctrl_state_offset = layout.ctrl_hdr_offset + kCtrlHdrSize;
  layout.ctrl_ack_offset = layout.ctrl_state_offset + kCtrlStateSize;
  layout.total_size =
      round_up_to_page(checked_add(layout.ctrl_ack_offset, kCtrlAckSize,
                                   "control command area"));
  return layout;
}

static void print_control_dma_layout(const ControlDmaLayout &layout,
                                     std::uint64_t base_iova,
                                     std::uint16_t rx_buffers,
                                     std::size_t rx_buffer_size) {
  std::println("DMA memory layout:");
  std::println("  RX vring offset: 0x{:x}", layout.rx_vring_offset);
  std::println("  RX vring IOVA: 0x{:x}", base_iova + layout.rx_vring_offset);
  std::println("  RX vring bytes: {}", layout.rx_vring.total_size);
  std::println("  TX vring offset: 0x{:x}", layout.tx_vring_offset);
  std::println("  TX vring IOVA: 0x{:x}", base_iova + layout.tx_vring_offset);
  std::println("  TX vring bytes: {}", layout.tx_vring.total_size);
  std::println("  control vring offset: 0x{:x}", layout.ctrl_vring_offset);
  std::println("  control vring IOVA: 0x{:x}",
               base_iova + layout.ctrl_vring_offset);
  std::println("  control vring bytes: {}", layout.ctrl_vring.total_size);
  std::println("  RX buffers offset: 0x{:x}", layout.rx_buffers_offset);
  std::println("  RX buffers IOVA: 0x{:x}",
               base_iova + layout.rx_buffers_offset);
  std::println("  RX buffers: {} x {} bytes", rx_buffers, rx_buffer_size);
  std::println("  RX buffer bytes: {}", layout.rx_buffers_size);
  std::println("  control command offset: 0x{:x}",
               layout.ctrl_command_offset);
  std::println("  control header IOVA: 0x{:x}",
               base_iova + layout.ctrl_hdr_offset);
  std::println("  control state IOVA: 0x{:x}",
               base_iova + layout.ctrl_state_offset);
  std::println("  control ACK IOVA: 0x{:x}",
               base_iova + layout.ctrl_ack_offset);
  std::println("  mapped bytes: {}", layout.total_size);
}

static std::vector<std::uint32_t>
negotiate_control_features(MappedRegion &mapping, std::size_t mapping_delta) {
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

  if (!feature_is_set(device_features.words, VIRTIO_NET_F_CTRL_VQ)) {
    throw std::runtime_error(
        "device does not offer VIRTIO_NET_F_CTRL_VQ; no control virtqueue");
  }
  if (!feature_is_set(device_features.words, VIRTIO_NET_F_CTRL_RX)) {
    throw std::runtime_error(
        "device does not offer VIRTIO_NET_F_CTRL_RX; cannot send RX mode "
        "control commands");
  }

  std::vector<std::uint32_t> guest_features =
      minimal_guest_features(device_features.words);
  set_feature(guest_features, VIRTIO_NET_F_CTRL_VQ);
  set_feature(guest_features, VIRTIO_NET_F_CTRL_RX);
  print_feature_words("Guest feature words", guest_features);
  write_guest_feature_words(mapping, mapping_delta, guest_features);

  constexpr std::uint8_t features_ok_status =
      VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
      VIRTIO_CONFIG_S_FEATURES_OK;
  write_status(mapping, mapping_delta, features_ok_status);

  std::uint8_t after_features_ok = read_status(mapping, mapping_delta);
  print_status("after FEATURES_OK", after_features_ok);
  if (!(after_features_ok & VIRTIO_CONFIG_S_FEATURES_OK))
    throw std::runtime_error("device rejected FEATURES_OK");

  return guest_features;
}

static void publish_promisc_control_command(std::uint8_t *ctrl_queue_base,
                                            std::uint8_t *dma_base,
                                            const ControlDmaLayout &layout,
                                            std::uint64_t base_iova,
                                            bool promisc) {
  std::uint8_t *header = dma_base + layout.ctrl_hdr_offset;
  std::uint8_t *state = dma_base + layout.ctrl_state_offset;
  std::uint8_t *ack = dma_base + layout.ctrl_ack_offset;

  header[0] = kVirtioNetCtrlRx;
  header[1] = kVirtioNetCtrlRxPromisc;
  state[0] = promisc ? 1 : 0;
  ack[0] = kCtrlAckInitial;

  VringDesc *desc = vring_descs(ctrl_queue_base, layout.ctrl_vring);
  desc[0].addr = base_iova + layout.ctrl_hdr_offset;
  desc[0].len = kCtrlHdrSize;
  desc[0].flags = kVringDescFNext;
  desc[0].next = 1;
  desc[1].addr = base_iova + layout.ctrl_state_offset;
  desc[1].len = kCtrlStateSize;
  desc[1].flags = kVringDescFNext;
  desc[1].next = 2;
  desc[2].addr = base_iova + layout.ctrl_ack_offset;
  desc[2].len = kCtrlAckSize;
  desc[2].flags = kVringDescFWrite;
  desc[2].next = 0;

  std::uint16_t *avail_ring = vring_avail_ring(ctrl_queue_base,
                                               layout.ctrl_vring);
  avail_ring[0] = 0;
  std::atomic_thread_fence(std::memory_order_release);
  *vring_avail_idx(ctrl_queue_base, layout.ctrl_vring) = 1;
  std::atomic_thread_fence(std::memory_order_release);
}

static void print_notify_target(std::string_view label, const VirtioCap &notify,
                                std::uint64_t bar_offset, off_t mmap_offset,
                                std::size_t delta) {
  std::println("{} notify write target:", label);
  std::println("  BAR{} relative offset: 0x{:x}", notify.bar, bar_offset);
  std::println("  VFIO device-fd file offset: 0x{:x}",
               mmap_offset + static_cast<off_t>(delta));
  std::println("  mmap file offset: 0x{:x}", mmap_offset);
  std::println("  mmap delta: 0x{:x}", delta);
}

static void dry_run(const std::string &bdf, const fs::path &dev_dir,
                    const Options &options) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);
  std::println("RX queue: {}", options.rx_queue);
  std::println("TX queue: {}", options.tx_queue);
  std::println("control queue: {}", options.ctrl_queue);
  std::println("base IOVA: 0x{:x}", options.iova);
  std::println("vring alignment: {}", options.align);
  std::println("RX buffers: {} x {} bytes", options.rx_buffers,
               options.rx_buffer_size);
  std::println("wait-ms: {}", options.wait_ms);
  std::println("promisc: {}", on_off(options.promisc));

  if (options.queue_size) {
    ControlDmaLayout layout = compute_control_dma_layout(
        *options.queue_size, *options.queue_size, *options.queue_size,
        options.align, options.rx_buffers, options.rx_buffer_size);
    print_control_dma_layout(layout, options.iova, options.rx_buffers,
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
               "VIRTIO_NET_F_CTRL_VQ and VIRTIO_NET_F_CTRL_RX");
  std::println("Would inspect RX queue {}, TX queue {}, and control queue {}",
               options.rx_queue, options.tx_queue, options.ctrl_queue);
  std::println("Would allocate and VFIO-map one DMA area for three vrings, RX "
               "buffers, and the control command");
  std::println("Would publish RX buffers");
  std::println("Would publish control descriptor chain: header -> state -> "
               "ACK");
  std::println("Would enable RX/TX/control queues");
  std::println("Would write DRIVER_OK, notify RX, notify control queue, and "
               "wait up to {} ms for control completion",
               options.wait_ms);
  std::println("Would print control ACK and reset the device before exit");
  std::println("Pass --yes to perform these device and DMA writes.");
}

static QueueView inspect_and_choose_queue(
    MappedRegion &mapping, std::size_t mapping_delta,
    QueueSelectionGuard &selection, std::uint16_t queue,
    std::string_view label, std::optional<std::uint16_t> requested,
    std::uint16_t &chosen_size) {
  selection.select(queue);
  QueueView view = read_queue_view(mapping, mapping_delta);
  print_queue_view(std::string(label) + " before sizing", view);
  if (view.enable != 0) {
    throw std::runtime_error(std::string(label) +
                             " is already enabled; reset the device before "
                             "using this tutorial sample");
  }
  chosen_size = choose_queue_size(view.size, requested);
  if (chosen_size != view.size) {
    std::println("using reduced {} queue_size: {} (device max {})", label,
                 chosen_size, view.size);
  }
  return view;
}

static void run_control_command(const std::string &bdf,
                                const fs::path &dev_dir,
                                const Options &options) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);
  std::println("RX queue: {}", options.rx_queue);
  std::println("TX queue: {}", options.tx_queue);
  std::println("control queue: {}", options.ctrl_queue);
  std::println("base IOVA: 0x{:x}", options.iova);
  std::println("vring alignment: {}", options.align);
  std::println("RX buffers: {} x {} bytes", options.rx_buffers,
               options.rx_buffer_size);
  std::println("wait-ms: {}", options.wait_ms);
  std::println("control command: RX_PROMISC={}", on_off(options.promisc));

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
  std::println("  notify_off_multiplier: {}", notify.notify_off_multiplier);

  std::size_t mapping_delta = 0;
  MappedRegion common_mapping =
      map_common_cfg(vfio.device.get(), common, mapping_delta, true);
  std::size_t device_cfg_delta = 0;
  MappedRegion device_cfg_mapping =
      map_device_cfg(vfio.device.get(), device_cfg, device_cfg_delta,
                     kVirtioNetConfigMinBytes);

  print_status("initial device_status",
               read_status(common_mapping, mapping_delta));
  std::vector<std::uint32_t> guest_features =
      negotiate_control_features(common_mapping, mapping_delta);

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
  if (options.ctrl_queue >= num_queues)
    throw std::runtime_error("control queue index is outside num_queues");
  if (options.rx_queue == options.tx_queue ||
      options.rx_queue == options.ctrl_queue ||
      options.tx_queue == options.ctrl_queue) {
    throw std::runtime_error("RX, TX, and control queues must be different");
  }

  QueueSelectionGuard selection(common_mapping, mapping_delta, original_select);

  std::uint16_t rx_queue_size = 0;
  std::uint16_t tx_queue_size = 0;
  std::uint16_t ctrl_queue_size = 0;
  inspect_and_choose_queue(common_mapping, mapping_delta, selection,
                           options.rx_queue, "RX queue", options.queue_size,
                           rx_queue_size);
  inspect_and_choose_queue(common_mapping, mapping_delta, selection,
                           options.tx_queue, "TX queue", options.queue_size,
                           tx_queue_size);
  inspect_and_choose_queue(common_mapping, mapping_delta, selection,
                           options.ctrl_queue, "control queue",
                           options.queue_size, ctrl_queue_size);

  ControlDmaLayout layout = compute_control_dma_layout(
      rx_queue_size, tx_queue_size, ctrl_queue_size, options.align,
      options.rx_buffers, options.rx_buffer_size);
  print_control_dma_layout(layout, options.iova, options.rx_buffers,
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
  std::uint8_t *ctrl_queue_base = buffer.data() + layout.ctrl_vring_offset;
  std::uint64_t rx_queue_iova = options.iova + layout.rx_vring_offset;
  std::uint64_t tx_queue_iova = options.iova + layout.tx_vring_offset;
  std::uint64_t ctrl_queue_iova = options.iova + layout.ctrl_vring_offset;
  std::uint64_t rx_buffer_iova = options.iova + layout.rx_buffers_offset;

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

  publish_promisc_control_command(ctrl_queue_base, buffer.data(), layout,
                                  options.iova, options.promisc);
  std::println("Published control descriptor chain:");
  std::println("  desc 0: control header class={} cmd={}", kVirtioNetCtrlRx,
               kVirtioNetCtrlRxPromisc);
  std::println("  desc 1: RX_PROMISC state={}", options.promisc ? 1 : 0);
  std::println("  desc 2: writable ACK byte initialized to 0x{:02x}",
               kCtrlAckInitial);
  std::println("control avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(ctrl_queue_base, layout.ctrl_vring)));
  std::println("Initial control used.idx: {}",
               read_used_idx(ctrl_queue_base, layout.ctrl_vring));

  QueueView rx_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.rx_queue,
      layout.rx_vring, rx_queue_iova, "RX queue");
  QueueView tx_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.tx_queue,
      layout.tx_vring, tx_queue_iova, "TX queue");
  QueueView ctrl_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.ctrl_queue,
      layout.ctrl_vring, ctrl_queue_iova, "control queue");

  std::size_t rx_notify_delta = 0;
  std::uint64_t rx_notify_bar_offset = 0;
  off_t rx_notify_mmap_offset = 0;
  MappedRegion rx_notify_mapping = map_notify_window(
      vfio.device.get(), notify, notify_region, rx_after_enable.notify_off,
      rx_notify_delta, rx_notify_bar_offset, rx_notify_mmap_offset);
  print_notify_target("RX", notify, rx_notify_bar_offset,
                      rx_notify_mmap_offset, rx_notify_delta);

  std::size_t ctrl_notify_delta = 0;
  std::uint64_t ctrl_notify_bar_offset = 0;
  off_t ctrl_notify_mmap_offset = 0;
  MappedRegion ctrl_notify_mapping = map_notify_window(
      vfio.device.get(), notify, notify_region, ctrl_after_enable.notify_off,
      ctrl_notify_delta, ctrl_notify_bar_offset, ctrl_notify_mmap_offset);
  print_notify_target("control", notify, ctrl_notify_bar_offset,
                      ctrl_notify_mmap_offset, ctrl_notify_delta);

  std::uint16_t rx_notify_value = rx_after_enable.selected;
  std::uint16_t ctrl_notify_value = ctrl_after_enable.selected;
  if (rx_after_enable.notify_data)
    std::println("  RX queue_notify_data candidate: {}",
                 *rx_after_enable.notify_data);
  if (ctrl_after_enable.notify_data)
    std::println("  control queue_notify_data candidate: {}",
                 *ctrl_after_enable.notify_data);
  std::println("  RX notify value: {} (queue index; "
               "VIRTIO_F_NOTIFICATION_DATA was not negotiated)",
               rx_notify_value);
  std::println("  control notify value: {} (queue index; "
               "VIRTIO_F_NOTIFICATION_DATA was not negotiated)",
               ctrl_notify_value);
  std::println("TX queue enabled but not notified; it is present only to keep "
               "the queue pair valid");
  (void)tx_after_enable;

  std::uint16_t ctrl_used_base_idx =
      read_used_idx(ctrl_queue_base, layout.ctrl_vring);
  std::atomic_thread_fence(std::memory_order_release);
  set_driver_ok(common_mapping, mapping_delta);
  print_status("after DRIVER_OK", read_status(common_mapping, mapping_delta));

  write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
  std::println("Wrote one 16-bit RX notify value");
  write_notify(ctrl_notify_mapping, ctrl_notify_delta, ctrl_notify_value);
  std::println("Wrote one 16-bit control notify value");
  std::println("Waiting up to {} ms for control completion", options.wait_ms);

  if (options.wait_ms > 0) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::uint16_t current =
          read_used_idx(ctrl_queue_base, layout.ctrl_vring);
      if (used_delta(ctrl_used_base_idx, current) >= 1)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  std::uint16_t ctrl_used_after_wait =
      read_used_idx(ctrl_queue_base, layout.ctrl_vring);
  std::println("control used.idx after wait: {} (delta {})",
               ctrl_used_after_wait,
               used_delta(ctrl_used_base_idx, ctrl_used_after_wait));

  if (used_delta(ctrl_used_base_idx, ctrl_used_after_wait) >= 1) {
    const volatile VringUsedElem *elems =
        vring_used_elems(ctrl_queue_base, layout.ctrl_vring);
    std::uint16_t used_slot =
        ctrl_used_base_idx % layout.ctrl_vring.queue_size;
    std::uint32_t id = static_cast<std::uint32_t>(elems[used_slot].id);
    std::uint32_t len = static_cast<std::uint32_t>(elems[used_slot].len);
    std::println("control used entry:");
    std::println("  slot: {}", used_slot);
    std::println("  id: {}", id);
    std::println("  len: {}", len);
  } else {
    std::println("control used entries: <none>");
  }

  std::atomic_thread_fence(std::memory_order_acquire);
  std::uint8_t ack = *(buffer.data() + layout.ctrl_ack_offset);
  std::println("control ACK byte: 0x{:02x} ({})", ack, ctrl_ack_name(ack));
  if (ack != kVirtioNetOk) {
    std::println("Control command did not return VIRTIO_NET_OK");
  }
  print_status("after wait", read_status(common_mapping, mapping_delta));

  selection.restore();
  std::println("Restored queue_select to {}", original_select);

  reset_device(common_mapping, mapping_delta, "after cleanup reset");
  reset_guard.disarm();
  pci_command.restore();

  std::uint64_t unmapped = dma.unmap();
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped",
               options.iova, unmapped);
  std::println("Control command sample cleaned up the device.");
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
      } else if (arg == "--ctrl-queue" && i + 1 < argc) {
        options.ctrl_queue = parse_u16(argv[++i], "ctrl-queue");
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
      } else if (arg == "--promisc" && i + 1 < argc) {
        options.promisc = parse_on_off(argv[++i], "promisc");
      } else if (arg == "--dry-run") {
        options.yes = false;
      } else if (arg == "--yes") {
        options.yes = true;
      } else {
        usage(argv[0]);
        return 1;
      }
    }

    if (!is_power_of_two(options.align) ||
        options.align < kVringUsedAlignSize) {
      throw std::runtime_error(
          "align must be a power of two and at least 4 bytes");
    }

    if (options.wait_ms == 0)
      throw std::runtime_error("wait-ms must be greater than 0");

    if (options.yes)
      run_control_command(bdf, dev_dir, options);
    else
      dry_run(bdf, dev_dir, options);

    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
