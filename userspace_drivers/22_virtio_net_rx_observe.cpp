// Exercise 22: Observe one virtio-net RX packet through VFIO
//
// Goal: keep the first virtio-net RX/TX queue pair running long enough to
// observe RX used-ring entries, then dump the received buffer prefix.
//
// Background
// ----------
// Exercise 21 proved that providing RX buffers avoids the immediate
// NEEDS_RESET seen with an empty queue.  This sample keeps that setup alive
// a little longer and inspects the first buffers the device returns:
//
//   reset -> ACKNOWLEDGE -> DRIVER -> FEATURES_OK
//   configure RX queue 0
//   configure TX queue 1
//   fill RX descriptors with writable packet buffers
//   publish RX descriptors through avail.idx
//   enable both queues
//   write DRIVER_OK
//   notify RX queue
//   poll RX used.idx until a packet arrives or timeout expires
//   decode the used descriptor id
//   dump the virtio-net header and packet prefix
//
// It still does not transmit packets and it does not recycle used RX buffers.
// Before unmapping DMA memory, it resets the device so the device stops using
// the queues.
//
// New concepts
// ------------
// - Mapping used-ring entries back to descriptor ids
// - Interpreting the virtio-net RX header boundary
// - Dumping received packet bytes from userspace DMA memory
// - The difference between "queue is running" and "a real packet arrived"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

constexpr std::uint16_t kRxObserveDefaultRxBuffers = 32;
constexpr std::size_t kRxObserveDefaultDumpBytes = 96;

struct Options {
  std::uint16_t rx_queue = kDefaultRxQueue;
  std::uint16_t tx_queue = kDefaultTxQueue;
  std::optional<std::uint16_t> queue_size;
  std::uint64_t iova = 0x100000000ull;
  std::size_t align = 4096;
  std::uint16_t rx_buffers = kRxObserveDefaultRxBuffers;
  std::size_t rx_buffer_size = kDefaultRxBufferSize;
  std::uint32_t wait_ms = 10000;
  std::uint16_t stop_after = 1;
  std::size_t dump_bytes = kRxObserveDefaultDumpBytes;
  bool yes = false;
};

static void usage(const char *argv0) {
  std::println(std::cerr,
               "Usage:\n"
               "  {} <BDF> [--rx-queue <n>] [--tx-queue <n>]\n"
               "       [--queue-size <n>] [--rx-buffers <n>]\n"
               "       [--rx-buffer-size <bytes>] [--iova <addr>]\n"
               "       [--align <bytes>] [--wait-ms <n>]\n"
               "       [--stop-after <n>] [--dump-bytes <n>]\n"
               "       [--dry-run|--yes]\n\n"
               "Defaults:\n"
               "  rx-queue = 0\n"
               "  tx-queue = 1\n"
               "  queue-size = each target queue's device-reported size\n"
               "  rx-buffers = 32\n"
               "  rx-buffer-size = 2048\n"
               "  iova = 0x100000000\n"
               "  align = 4096\n"
               "  wait-ms = 10000\n"
               "  stop-after = 1\n"
               "  dump-bytes = 96\n"
               "  default mode = dry-run\n\n"
               "Examples:\n"
               "  {} c1:00.6\n"
               "  {} c1:00.6 --queue-size 128 --rx-buffer-size 4096 --wait-ms 10000 --yes\n"
               "  {} c1:00.6 --rx-buffers 64 --stop-after 4 --dump-bytes 128 --yes",
               argv0, argv0, argv0, argv0);
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
  std::println("stop-after: {} RX used entries", options.stop_after);
  std::println("dump-bytes: {}", options.dump_bytes);

  if (options.queue_size) {
    DmaLayout layout = compute_dma_layout(*options.queue_size,
                                          *options.queue_size, options.align,
                                          options.rx_buffers,
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
               "RX packet buffers");
  std::println("Would publish writable RX descriptors and set RX avail.idx");
  std::println("Would write queue_size, queue_desc, queue_avail, queue_used "
               "for both queues");
  std::println("Would enable both queues");
  std::println("Would write DRIVER_OK, notify RX, and poll used.idx for {} ms",
               options.wait_ms);
  std::println("Would stop after {} RX used entries and dump up to {} bytes "
               "from each returned RX buffer",
               options.stop_after, options.dump_bytes);
  std::println("Would restore queue_select, reset the device, then unmap DMA");
  std::println("Would not transmit packets or recycle used RX buffers.");
  std::println("Pass --yes to perform these device and DMA writes.");
}

static void run_rx_observe(const std::string &bdf, const fs::path &dev_dir,
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
  std::println("stop-after: {} RX used entries", options.stop_after);
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

  DmaLayout layout = compute_dma_layout(rx_queue_size, tx_queue_size,
                                        options.align, options.rx_buffers,
                                        options.rx_buffer_size);
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
  std::uint64_t rx_queue_iova = options.iova + layout.rx_vring_offset;
  std::uint64_t tx_queue_iova = options.iova + layout.tx_vring_offset;
  std::uint64_t rx_buffer_iova = options.iova + layout.rx_buffers_offset;

  publish_rx_buffers(rx_queue_base, layout.rx_vring, rx_buffer_iova,
                     options.rx_buffers, options.rx_buffer_size);
  std::println("Published {} writable RX descriptors", options.rx_buffers);
  std::println("RX avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(rx_queue_base, layout.rx_vring)));
  std::println("Initial RX used.idx: {}",
               read_used_idx(rx_queue_base, layout.rx_vring));

  QueueView rx_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.rx_queue,
      layout.rx_vring, rx_queue_iova, "RX queue");
  QueueView tx_after_enable = program_and_enable_queue(
      common_mapping, mapping_delta, selection, options.tx_queue,
      layout.tx_vring, tx_queue_iova, "TX queue");
  (void)tx_after_enable;

  std::size_t notify_delta = 0;
  std::uint64_t notify_bar_offset = 0;
  off_t notify_mmap_offset = 0;
  MappedRegion notify_mapping = map_notify_window(
      vfio.device.get(), notify, notify_region, rx_after_enable.notify_off,
      notify_delta, notify_bar_offset, notify_mmap_offset);
  std::println("RX notify write target:");
  std::println("  BAR{} relative offset: 0x{:x}", notify.bar,
               notify_bar_offset);
  std::println("  VFIO device-fd file offset: 0x{:x}",
               notify_mmap_offset + static_cast<off_t>(notify_delta));
  std::println("  mmap file offset: 0x{:x}", notify_mmap_offset);
  std::println("  mmap delta: 0x{:x}", notify_delta);

  std::uint16_t notify_value = rx_after_enable.selected;
  if (rx_after_enable.notify_data) {
    std::println("  queue_notify_data candidate: {}",
                 *rx_after_enable.notify_data);
  }
  std::println("  notify value: {} (queue index; "
               "VIRTIO_F_NOTIFICATION_DATA was not negotiated)",
               notify_value);

  std::uint16_t used_base_idx = read_used_idx(rx_queue_base, layout.rx_vring);
  std::atomic_thread_fence(std::memory_order_release);
  set_driver_ok(common_mapping, mapping_delta);
  print_status("after DRIVER_OK", read_status(common_mapping, mapping_delta));

  write_notify(notify_mapping, notify_delta, notify_value);
  std::println("Wrote one 16-bit RX notify value");
  std::println("Waiting up to {} ms for {} RX used entries", options.wait_ms,
               options.stop_after);

  if (options.wait_ms > 0) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::uint16_t current = read_used_idx(rx_queue_base, layout.rx_vring);
      if (used_delta(used_base_idx, current) >= options.stop_after)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::uint16_t used_after_wait = read_used_idx(rx_queue_base, layout.rx_vring);
  std::println("RX used.idx after wait: {} (delta {})", used_after_wait,
               used_delta(used_base_idx, used_after_wait));
  print_rx_used_entries(rx_queue_base, layout.rx_vring, used_base_idx,
                        used_after_wait);
  dump_rx_used_buffers(buffer.data(), rx_queue_base, layout, used_base_idx,
                       used_after_wait, options.rx_buffers,
                       options.rx_buffer_size, options.stop_after,
                       options.dump_bytes);
  print_status("after wait", read_status(common_mapping, mapping_delta));

  selection.restore();
  std::println("Restored queue_select to {}", original_select);

  reset_device(common_mapping, mapping_delta, "after cleanup reset");
  reset_guard.disarm();
  pci_command.restore();

  std::uint64_t unmapped = dma.unmap();
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped",
               options.iova, unmapped);
  std::println("Did not transmit packets or recycle used RX buffers.");
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
      } else if (arg == "--stop-after" && i + 1 < argc) {
        options.stop_after = parse_u16(argv[++i], "stop-after");
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
    if (options.stop_after == 0)
      throw std::runtime_error("stop-after must be greater than 0");
    if (options.stop_after > options.rx_buffers)
      throw std::runtime_error("stop-after must not exceed rx-buffers");

    if (options.yes)
      run_rx_observe(bdf, dev_dir, options);
    else
      dry_run(bdf, dev_dir, options);

    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
