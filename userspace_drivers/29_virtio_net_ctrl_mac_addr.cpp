// Exercise 29: Set virtio-net MAC address through the control virtqueue
//
// Goal: send VIRTIO_NET_CTRL_MAC_ADDR_SET through the control virtqueue, keep
// the device running, and observe whether frames sent to the temporary MAC
// address are returned through the RX used ring.
//
// Background
// ----------
// Exercise 27 sent a one-byte RX_PROMISC control command.  Exercise 28 kept
// that side effect active long enough to observe RX behavior.  This sample uses
// the same control queue mechanism for a different command:
//
//   class = VIRTIO_NET_CTRL_MAC
//   cmd   = VIRTIO_NET_CTRL_MAC_ADDR_SET
//   data  = 6-byte MAC address
//   ack   = writable ACK byte
//
// The command requires VIRTIO_NET_F_CTRL_MAC_ADDR.  The sample resets the
// device on exit, so the temporary MAC address is not left behind.
//
// New concepts
// ------------
// - Negotiating VIRTIO_NET_F_CTRL_MAC_ADDR
// - Sending a control command with a 6-byte data payload
// - Testing whether the device accepts packets for the temporary MAC address

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <linux/vfio.h>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "virtio_net_vfio.hpp"

namespace fs = std::filesystem;

constexpr std::array<std::uint8_t, 6> kDefaultTemporaryMac{
    0x02, 0x00, 0x00, 0x00, 0x29, 0x01};

struct Options {
  std::uint16_t rx_queue = kDefaultRxQueue;
  std::uint16_t tx_queue = kDefaultTxQueue;
  std::uint16_t ctrl_queue = kDefaultCtrlQueue;
  std::optional<std::uint16_t> queue_size;
  std::uint64_t iova = 0x100000000ull;
  std::size_t align = 4096;
  std::uint16_t rx_buffers = kDefaultRxBuffers;
  std::size_t rx_buffer_size = kDefaultRxBufferSize;
  std::uint32_t wait_ms = 60000;
  std::uint16_t stop_after = 1;
  std::size_t dump_bytes = kDefaultDumpBytes;
  std::optional<std::uint16_t> match_ethertype = kDefaultEthertype;
  std::array<std::uint8_t, 6> new_mac = kDefaultTemporaryMac;
  bool dump_every_packet = false;
  bool yes = false;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} <BDF> [--new-mac <mac>]\n"
      "       [--rx-queue <n>] [--tx-queue <n>] [--ctrl-queue <n>]\n"
      "       [--queue-size <n>] [--rx-buffers <n>]\n"
      "       [--rx-buffer-size <bytes>] [--iova <addr>]\n"
      "       [--align <bytes>] [--wait-ms <n>] [--stop-after <n>]\n"
      "       [--match-ethertype <hex>|--accept-any-ethertype]\n"
      "       [--dump-bytes <n>] [--dump-every-packet]\n"
      "       [--dry-run|--yes]\n\n"
      "Defaults:\n"
      "  new-mac = 02:00:00:00:29:01\n"
      "  rx-queue = 0\n"
      "  tx-queue = 1\n"
      "  ctrl-queue = 2\n"
      "  queue-size = each target queue's device-reported size\n"
      "  rx-buffers = 64\n"
      "  rx-buffer-size = 2048\n"
      "  iova = 0x100000000\n"
      "  align = 4096\n"
      "  wait-ms = 60000\n"
      "  stop-after = 1\n"
      "  match-ethertype = 0x88b5\n"
      "  dump-bytes = 160\n"
      "  dump-every-packet = false\n"
      "  default mode = dry-run\n\n"
      "Examples:\n"
      "  {} c1:00.6\n"
      "  {} c1:00.6 --new-mac 02:00:00:00:29:01 --wait-ms 60000 --yes\n"
      "  {} c1:00.6 --new-mac 02:00:00:00:29:02 --accept-any-ethertype --yes",
      argv0, argv0, argv0, argv0);
}

static DmaLayout rx_view_layout(const CtrlqDmaLayout &layout) {
  DmaLayout view;
  view.rx_vring = layout.rx_vring;
  view.tx_vring = layout.tx_vring;
  view.rx_vring_offset = layout.rx_vring_offset;
  view.tx_vring_offset = layout.tx_vring_offset;
  view.rx_buffers_offset = layout.rx_buffers_offset;
  view.rx_buffers_size = layout.rx_buffers_size;
  view.total_size = layout.total_size;
  return view;
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

static std::uint8_t wait_for_control_ack(std::uint8_t *dma_base,
                                         std::uint8_t *ctrl_queue_base,
                                         const CtrlqDmaLayout &layout,
                                         std::uint16_t base_idx,
                                         std::uint32_t wait_ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::uint16_t current = read_used_idx(ctrl_queue_base, layout.ctrl_vring);
    if (used_delta(base_idx, current) >= 1)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::uint16_t after = read_used_idx(ctrl_queue_base, layout.ctrl_vring);
  std::println("control used.idx after wait: {} (delta {})", after,
               used_delta(base_idx, after));
  print_used_entries("control", ctrl_queue_base, layout.ctrl_vring, base_idx,
                     after);

  std::atomic_thread_fence(std::memory_order_acquire);
  return *(dma_base + layout.ctrl_ack_offset);
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
  std::println("temporary MAC: {}", mac_string(options.new_mac));
  std::println("wait-ms: {}", options.wait_ms);
  std::println("stop-after: {} matching RX packets", options.stop_after);
  std::println("match ethertype: {}",
               options.match_ethertype
                   ? std::format("0x{:04x}", *options.match_ethertype)
                   : std::string("any"));
  std::println("dump-bytes: {}", options.dump_bytes);
  std::println("dump-every-packet: {}",
               options.dump_every_packet ? "yes" : "no");

  if (options.queue_size) {
    CtrlqDmaLayout layout = compute_ctrlq_dma_layout(
        *options.queue_size, *options.queue_size, *options.queue_size,
        options.align, options.rx_buffers, options.rx_buffer_size,
        kVirtioNetCtrlMacAddrSize);
    print_ctrlq_dma_layout(layout, options.iova, options.rx_buffers,
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
               "VIRTIO_NET_F_CTRL_VQ and VIRTIO_NET_F_CTRL_MAC_ADDR");
  std::println("Would publish RX buffers and a MAC_ADDR_SET control command");
  std::println("Would enable RX/TX/control queues");
  std::println("Would write DRIVER_OK, notify RX, notify control, wait for "
               "control ACK, then observe RX for {} ms",
               options.wait_ms);
  std::println("Would print whether each RX packet's destination MAC matches "
               "the temporary MAC");
  std::println("Would reset the device before exit, so the temporary MAC does "
               "not persist");
  std::println("Pass --yes to perform these device and DMA writes.");
}

static void run_mac_addr_sample(const std::string &bdf,
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
  std::println("control command: MAC_ADDR_SET={}",
               mac_string(options.new_mac));
  std::println("wait-ms: {}", options.wait_ms);
  std::println("stop-after: {} matching RX packets", options.stop_after);
  std::println("match ethertype: {}",
               options.match_ethertype
                   ? std::format("0x{:04x}", *options.match_ethertype)
                   : std::string("any"));
  std::println("dump-bytes: {}", options.dump_bytes);
  std::println("dump-every-packet: {}",
               options.dump_every_packet ? "yes" : "no");

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
      negotiate_ctrl_mac_addr_features(common_mapping, mapping_delta);

  VirtioNetConfigView initial_config = read_virtio_net_config(
      device_cfg_mapping, device_cfg_delta, device_cfg.length);
  print_virtio_net_config("Initial virtio-net device config", initial_config,
                          guest_features);
  std::println("Temporary MAC requested through control queue: {}",
               mac_string(options.new_mac));

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

  CtrlqDmaLayout layout = compute_ctrlq_dma_layout(
      rx_queue_size, tx_queue_size, ctrl_queue_size, options.align,
      options.rx_buffers, options.rx_buffer_size, kVirtioNetCtrlMacAddrSize);
  DmaLayout rx_layout;
  rx_layout.rx_vring = layout.rx_vring;
  rx_layout.tx_vring = layout.tx_vring;
  rx_layout.rx_vring_offset = layout.rx_vring_offset;
  rx_layout.tx_vring_offset = layout.tx_vring_offset;
  rx_layout.rx_buffers_offset = layout.rx_buffers_offset;
  rx_layout.rx_buffers_size = layout.rx_buffers_size;
  rx_layout.total_size = layout.total_size;
  print_ctrlq_dma_layout(layout, options.iova, options.rx_buffers,
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

  publish_mac_addr_control_command(ctrl_queue_base, buffer.data(), layout,
                                   options.iova, options.new_mac);
  std::println("Published control descriptor chain:");
  std::println("  desc 0: control header class={} cmd={}", kVirtioNetCtrlMac,
               kVirtioNetCtrlMacAddrSet);
  std::println("  desc 1: MAC address data={}", mac_string(options.new_mac));
  std::println("  desc 2: writable ACK byte initialized to 0x{:02x}",
               kVirtioNetCtrlAckInitial);
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
  std::uint16_t last_rx_used_idx =
      read_used_idx(rx_queue_base, layout.rx_vring);
  std::atomic_thread_fence(std::memory_order_release);
  set_driver_ok(common_mapping, mapping_delta);
  print_status("after DRIVER_OK", read_status(common_mapping, mapping_delta));

  write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
  std::println("Wrote one 16-bit RX notify value");
  write_notify(ctrl_notify_mapping, ctrl_notify_delta, ctrl_notify_value);
  std::println("Wrote one 16-bit control notify value");
  std::println("Waiting up to {} ms for control completion", options.wait_ms);

  std::uint8_t ack =
      wait_for_control_ack(buffer.data(), ctrl_queue_base, layout,
                           ctrl_used_base_idx, options.wait_ms);
  std::println("control ACK byte: 0x{:02x} ({})", ack,
               virtio_net_ctrl_ack_name(ack));
  if (ack != kVirtioNetOk)
    throw std::runtime_error("control command did not return VIRTIO_NET_OK");

  VirtioNetConfigView after_ctrl_config = read_virtio_net_config(
      device_cfg_mapping, device_cfg_delta, device_cfg.length);
  print_virtio_net_config("Device config after MAC_ADDR_SET",
                          after_ctrl_config, guest_features);

  std::println("MAC_ADDR_SET accepted; observing RX for up to {} ms",
               options.wait_ms);
  std::println("Original config MAC: {}", mac_string(initial_config.mac));
  std::println("Temporary MAC: {}", mac_string(options.new_mac));

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(options.wait_ms);
  std::uint16_t matched = 0;
  std::uint32_t skipped = 0;
  while (matched < options.stop_after &&
         std::chrono::steady_clock::now() < deadline) {
    std::uint16_t current_rx_used =
        read_used_idx(rx_queue_base, layout.rx_vring);
    std::uint16_t count = used_delta(last_rx_used_idx, current_rx_used);
    if (count == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    const volatile VringUsedElem *elems =
        vring_used_elems(rx_queue_base, layout.rx_vring);
    for (std::uint16_t i = 0; i < count; ++i) {
      std::uint16_t used_slot = static_cast<std::uint16_t>(
          (last_rx_used_idx + i) % layout.rx_vring.queue_size);
      std::uint32_t id = static_cast<std::uint32_t>(elems[used_slot].id);

      std::optional<ReceivedPacket> received = read_rx_used_packet(
          buffer.data(), rx_queue_base, rx_layout, used_slot,
          options.rx_buffers, options.rx_buffer_size, options.match_ethertype);

      if (!received) {
        ++skipped;
        if (id < options.rx_buffers) {
          recycle_rx_buffer(rx_queue_base, layout.rx_vring,
                            static_cast<std::uint16_t>(id));
          write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
        }
        continue;
      }

      const std::uint8_t *ethernet =
          received->buffer + kVirtioNetHeaderSize;
      std::array<std::uint8_t, 6> dst = mac_from_bytes(ethernet);
      std::array<std::uint8_t, 6> src = mac_from_bytes(ethernet + 6);
      bool dst_is_original_mac = dst == initial_config.mac;
      bool dst_is_temporary_mac = dst == options.new_mac;

      ++matched;
      std::println("Observed RX packet {}:", matched);
      std::println("  used slot: {}", used_slot);
      std::println("  desc id: {}", received->desc_id);
      std::println("  used len: {}", received->used_len);
      std::println("  Ethernet dst: {}", mac_string(dst));
      std::println("  Ethernet src: {}", mac_string(src));
      std::println("  ethertype: 0x{:04x}", received->ethertype);
      std::println("  dst matches original config MAC: {}",
                   dst_is_original_mac ? "yes" : "no");
      std::println("  dst matches temporary MAC: {}",
                   dst_is_temporary_mac ? "yes" : "no");
      if (dst_is_temporary_mac) {
        std::println("  this is the expected MAC_ADDR_SET-path observation");
      }
      if ((matched == 1 || options.dump_every_packet) &&
          options.dump_bytes > 0) {
        std::println("RX buffer dump:");
        print_virtio_net_rx_buffer(received->buffer, received->used_len,
                                   options.dump_bytes);
      }

      recycle_rx_buffer(rx_queue_base, layout.rx_vring,
                        static_cast<std::uint16_t>(received->desc_id));
      write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);

      if (matched >= options.stop_after)
        break;
    }

    last_rx_used_idx = current_rx_used;
  }

  std::println("MAC address observe summary:");
  std::println("  matching RX packets: {}", matched);
  std::println("  skipped RX used entries: {}", skipped);
  std::println("  final RX avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(rx_queue_base, layout.rx_vring)));
  std::println("  final RX used.idx: {}",
               read_used_idx(rx_queue_base, layout.rx_vring));
  print_status("after observe", read_status(common_mapping, mapping_delta));

  selection.restore();
  std::println("Restored queue_select to {}", original_select);

  reset_device(common_mapping, mapping_delta, "after cleanup reset");
  reset_guard.disarm();
  pci_command.restore();

  std::uint64_t unmapped = dma.unmap();
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped",
               options.iova, unmapped);
  std::println("MAC address control sample cleaned up the device.");
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
      if (arg == "--new-mac" && i + 1 < argc) {
        options.new_mac = parse_mac(argv[++i], "new-mac");
      } else if (arg == "--rx-queue" && i + 1 < argc) {
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
      } else if (arg == "--stop-after" && i + 1 < argc) {
        options.stop_after = parse_u16(argv[++i], "stop-after");
      } else if (arg == "--match-ethertype" && i + 1 < argc) {
        options.match_ethertype = parse_u16(argv[++i], "match-ethertype");
      } else if (arg == "--accept-any-ethertype") {
        options.match_ethertype.reset();
      } else if (arg == "--dump-bytes" && i + 1 < argc) {
        options.dump_bytes =
            static_cast<std::size_t>(parse_ull(argv[++i], "dump-bytes"));
      } else if (arg == "--dump-every-packet") {
        options.dump_every_packet = true;
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
    if (options.stop_after == 0)
      throw std::runtime_error("stop-after must be greater than 0");

    if (options.yes)
      run_mac_addr_sample(bdf, dev_dir, options);
    else
      dry_run(bdf, dev_dir, options);

    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
