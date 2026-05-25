// Exercise 26: Drive the virtio-net echo loop with VFIO IRQ eventfds
//
// Goal: configure the first virtio-net RX/TX queue pair, assign MSI-X vectors
// to the queues, bind those vectors to eventfds through VFIO, and use poll()
// on the eventfds to drive the same bounded echo loop from exercise 25.
//
// Background
// ----------
// Exercise 25 used a sleep/poll loop over used.idx.  Real userspace drivers
// normally block until the kernel reports an interrupt event.  VFIO exposes
// device interrupts through eventfd:
//
//   eventfd() -> VFIO_DEVICE_SET_IRQS -> poll(eventfd)
//
// For modern virtio PCI, each virtqueue has a queue_msix_vector field in
// COMMON_CFG.  This sample writes a vector for RX queue 0 and TX queue 1,
// binds those MSI-X vectors to eventfds, then waits for eventfd signals before
// checking the RX/TX used rings.
//
// It is still a tutorial sample, not a production driver.  It handles only
// single-descriptor packets and keeps a bounded runtime.
//
// New concepts
// ------------
// - Assigning queue_msix_vector for virtqueues
// - Binding VFIO MSI-X vectors to eventfds
// - Using poll(eventfd) as the datapath wait primitive
// - Handling RX/TX ring work after interrupt delivery

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <linux/vfio.h>
#include <optional>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

#include "virtio_net_vfio.hpp"

namespace fs = std::filesystem;

struct Options {
  std::uint16_t rx_queue = kDefaultRxQueue;
  std::uint16_t tx_queue = kDefaultTxQueue;
  std::optional<std::uint16_t> queue_size;
  std::uint64_t iova = 0x100000000ull;
  std::size_t align = 4096;
  std::uint16_t rx_buffers = kDefaultRxBuffers;
  std::uint16_t tx_buffers = kDefaultTxBuffers;
  std::size_t rx_buffer_size = kDefaultRxBufferSize;
  std::uint32_t run_ms = 30000;
  std::uint32_t max_packets = 3;
  std::size_t dump_bytes = kDefaultDumpBytes;
  std::optional<std::array<std::uint8_t, 6>> src_mac;
  std::optional<std::uint16_t> match_ethertype = kDefaultEthertype;
  __u32 irq_index = VFIO_PCI_MSIX_IRQ_INDEX;
  std::uint16_t rx_vector = 0;
  std::uint16_t tx_vector = 1;
  bool dump_every_packet = false;
  bool yes = false;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} <BDF> [--rx-queue <n>] [--tx-queue <n>]\n"
      "       [--queue-size <n>] [--rx-buffers <n>] [--tx-buffers <n>]\n"
      "       [--rx-buffer-size <bytes>] [--iova <addr>]\n"
      "       [--align <bytes>] [--run-ms <n>] [--max-packets <n>]\n"
      "       [--src-mac <mac>]\n"
      "       [--match-ethertype <hex>|--accept-any-ethertype]\n"
      "       [--irq-index <n>] [--rx-vector <n>] [--tx-vector <n>]\n"
      "       [--dump-bytes <n>] [--dump-every-packet]\n"
      "       [--dry-run|--yes]\n\n"
      "Defaults:\n"
      "  rx-queue = 0\n"
      "  tx-queue = 1\n"
      "  queue-size = each target queue's device-reported size\n"
      "  rx-buffers = 64\n"
      "  tx-buffers = 8\n"
      "  rx-buffer-size = 2048\n"
      "  iova = 0x100000000\n"
      "  align = 4096\n"
      "  run-ms = 30000\n"
      "  max-packets = 3\n"
      "  src-mac = virtio-net config MAC\n"
      "  match-ethertype = 0x88b5\n"
      "  irq-index = 2 (MSI-X)\n"
      "  rx-vector = 0\n"
      "  tx-vector = 1\n"
      "  dump-bytes = 160\n"
      "  dump-every-packet = false\n"
      "  default mode = dry-run\n\n"
      "Examples:\n"
      "  {} c1:00.6\n"
      "  {} c1:00.6 --run-ms 60000 --max-packets 3 --yes\n"
      "  {} c1:00.6 --rx-vector 0 --tx-vector 1 --dump-every-packet --yes",
      argv0, argv0, argv0, argv0);
}

struct TxSlot {
  bool in_flight = false;
};

class IrqEventfdBinding {
public:
  explicit IrqEventfdBinding(std::uint16_t vector) : vector_(vector) {
    int raw_event_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (raw_event_fd == -1) {
      int error = errno;
      throw std::runtime_error("eventfd: " + errno_text(error));
    }
    event_.reset(raw_event_fd);
  }

  IrqEventfdBinding(const IrqEventfdBinding &) = delete;
  IrqEventfdBinding &operator=(const IrqEventfdBinding &) = delete;

  int fd() const { return event_.get(); }
  std::uint16_t vector() const { return vector_; }

  std::uint64_t drain() {
    std::uint64_t total = 0;
    for (;;) {
      std::uint64_t count = 0;
      ssize_t got = ::read(event_.get(), &count, sizeof(count));
      if (got == -1 && errno == EINTR)
        continue;
      if (got == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (got == -1) {
        int error = errno;
        throw std::runtime_error("read(eventfd): " + errno_text(error));
      }
      if (got != sizeof(count))
        throw std::runtime_error("short read from eventfd");
      total += count;
    }
    return total;
  }

private:
  std::uint16_t vector_ = 0;
  UniqueFd event_;
};

class IrqIndexBindingGuard {
public:
  IrqIndexBindingGuard(int device_fd, __u32 irq_index)
      : device_fd_(device_fd), irq_index_(irq_index) {}

  IrqIndexBindingGuard(const IrqIndexBindingGuard &) = delete;
  IrqIndexBindingGuard &operator=(const IrqIndexBindingGuard &) = delete;

  ~IrqIndexBindingGuard() { disable_noexcept(); }

  void activate() { active_ = true; }

  void disable() {
    if (!active_)
      return;
    vfio_disable_irq_index(device_fd_, irq_index_);
    active_ = false;
    std::println("Disabled IRQ index {} ({}) eventfd bindings", irq_index_,
                 vfio_irq_name(irq_index_));
  }

private:
  void disable_noexcept() noexcept {
    try {
      if (active_)
        vfio_disable_irq_index(device_fd_, irq_index_);
    } catch (...) {
    }
    active_ = false;
  }

  int device_fd_ = -1;
  __u32 irq_index_ = 0;
  bool active_ = false;
};

static bool consume_tx_completions(std::uint8_t *tx_queue_base,
                                   const VringLayout &tx_layout,
                                   std::uint16_t &last_tx_used_idx,
                                   std::vector<TxSlot> &tx_slots,
                                   std::uint32_t &completed_tx) {
  std::uint16_t current = read_used_idx(tx_queue_base, tx_layout);
  std::uint16_t count = used_delta(last_tx_used_idx, current);
  if (count == 0)
    return false;

  const volatile VringUsedElem *elems =
      vring_used_elems(tx_queue_base, tx_layout);
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint16_t slot = static_cast<std::uint16_t>((last_tx_used_idx + i) %
                                                    tx_layout.queue_size);
    std::uint32_t id = static_cast<std::uint32_t>(elems[slot].id);
    std::uint32_t len = static_cast<std::uint32_t>(elems[slot].len);
    if (id >= tx_slots.size()) {
      std::println("Skipping TX used slot {}: descriptor id {} is outside "
                   "the TX buffer pool",
                   slot, id);
      continue;
    }

    tx_slots[id].in_flight = false;
    ++completed_tx;
    std::println("TX completion: used slot {} desc id={} len={}", slot, id,
                 len);
  }

  last_tx_used_idx = current;
  return true;
}

static std::optional<std::uint16_t>
find_free_tx_slot(const std::vector<TxSlot> &tx_slots) {
  for (std::uint16_t i = 0; i < tx_slots.size(); ++i) {
    if (!tx_slots[i].in_flight)
      return i;
  }
  return std::nullopt;
}

static int timeout_until(std::chrono::steady_clock::time_point deadline) {
  auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;

  auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining.count() > std::numeric_limits<int>::max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(remaining.count());
}

static bool poll_irq_events(IrqEventfdBinding &rx_irq,
                            IrqEventfdBinding *tx_irq, int timeout_ms) {
  std::array<pollfd, 2> pfds{};
  nfds_t nfds = 1;
  pfds[0].fd = rx_irq.fd();
  pfds[0].events = POLLIN;
  if (tx_irq) {
    nfds = 2;
    pfds[1].fd = tx_irq->fd();
    pfds[1].events = POLLIN;
  }

  int ready = 0;
  for (;;) {
    ready = ::poll(pfds.data(), nfds, timeout_ms);
    if (ready == -1 && errno == EINTR)
      continue;
    if (ready == -1) {
      int error = errno;
      throw std::runtime_error("poll(eventfd): " + errno_text(error));
    }
    break;
  }

  if (ready == 0)
    return false;

  for (nfds_t i = 0; i < nfds; ++i) {
    if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      throw std::runtime_error("eventfd poll returned error events");
    }
  }

  std::uint64_t rx_count = rx_irq.drain();
  if (rx_count > 0) {
    std::println("IRQ vector {} eventfd signaled; counter={}", rx_irq.vector(),
                 rx_count);
  }
  if (tx_irq) {
    std::uint64_t tx_count = tx_irq->drain();
    if (tx_count > 0) {
      std::println("IRQ vector {} eventfd signaled; counter={}",
                   tx_irq->vector(), tx_count);
    }
  }

  return true;
}

static QueueView program_and_enable_queue_with_msix(
    MappedRegion &mapping, std::size_t mapping_delta,
    QueueSelectionGuard &selection, std::uint16_t queue,
    const VringLayout &layout, std::uint64_t queue_iova, std::uint16_t vector,
    std::string_view label) {
  selection.select(queue);
  QueueView before = read_queue_view(mapping, mapping_delta);
  print_queue_view(std::string(label) + " before programming", before);
  if (before.enable != 0) {
    throw std::runtime_error(std::string(label) +
                             " is already enabled; reset the device before "
                             "using this tutorial sample");
  }

  mapping.write16(mapping_delta + VIRTIO_PCI_COMMON_Q_MSIX, vector);
  QueueView after_vector = read_queue_view(mapping, mapping_delta);
  std::println("Wrote {} queue_msix_vector={}", label, vector);
  if (after_vector.msix_vector != vector) {
    throw std::runtime_error(std::string(label) +
                             " queue_msix_vector did not read back as the "
                             "requested vector");
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

static vfio_irq_info validate_irq_vectors(int device_fd,
                                          const Options &options) {
  vfio_device_info device_info = vfio_get_device_info(device_fd);
  if (options.irq_index >= device_info.num_irqs) {
    throw std::runtime_error(
        "IRQ index " + std::to_string(options.irq_index) +
        " is outside num_irqs=" + std::to_string(device_info.num_irqs));
  }

  vfio_irq_info irq = vfio_get_irq_info(device_fd, options.irq_index);
  std::println("IRQ index: {} ({})", options.irq_index,
               vfio_irq_name(options.irq_index));
  std::println("IRQ count: {}", irq.count);
  std::println("IRQ flags: {}", vfio_irq_flag_names(irq.flags));
  std::println("RX MSI-X vector: {}", options.rx_vector);
  std::println("TX MSI-X vector: {}", options.tx_vector);

  if (!(irq.flags & VFIO_IRQ_INFO_EVENTFD)) {
    throw std::runtime_error("IRQ index " + std::to_string(options.irq_index) +
                             " does not support eventfd delivery");
  }
  if (options.rx_vector >= irq.count) {
    throw std::runtime_error(
        "rx-vector " + std::to_string(options.rx_vector) +
        " is outside IRQ count=" + std::to_string(irq.count));
  }
  if (options.tx_vector >= irq.count) {
    throw std::runtime_error(
        "tx-vector " + std::to_string(options.tx_vector) +
        " is outside IRQ count=" + std::to_string(irq.count));
  }

  return irq;
}

static void bind_irq_eventfds(int device_fd, __u32 irq_index,
                              const vfio_irq_info &irq,
                              IrqEventfdBinding &rx_irq,
                              IrqEventfdBinding *tx_irq) {
  __u32 min_vector = rx_irq.vector();
  __u32 max_vector = rx_irq.vector();
  if (tx_irq) {
    min_vector = std::min<__u32>(min_vector, tx_irq->vector());
    max_vector = std::max<__u32>(max_vector, tx_irq->vector());
  }

  __u32 start = min_vector;
  __u32 count = max_vector - min_vector + 1;
  if (irq.flags & VFIO_IRQ_INFO_NORESIZE) {
    start = 0;
    count = irq.count;
  }

  std::vector<std::int32_t> event_fds(count, -1);
  event_fds[rx_irq.vector() - start] = rx_irq.fd();
  if (tx_irq)
    event_fds[tx_irq->vector() - start] = tx_irq->fd();

  vfio_set_irq_eventfds(device_fd, irq_index, start, event_fds);
  if (irq.flags & VFIO_IRQ_INFO_NORESIZE) {
    std::println("Bound IRQ index {} ({}) vectors in one NORESIZE setup "
                 "call: start={} count={}",
                 irq_index, vfio_irq_name(irq_index), start, count);
  }
  std::println("Bound IRQ index {} ({}) vector {} to eventfd {}", irq_index,
               vfio_irq_name(irq_index), rx_irq.vector(), rx_irq.fd());
  if (tx_irq) {
    std::println("Bound IRQ index {} ({}) vector {} to eventfd {}", irq_index,
                 vfio_irq_name(irq_index), tx_irq->vector(), tx_irq->fd());
  }
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
  std::println("TX buffers: {} x {} bytes", options.tx_buffers,
               options.rx_buffer_size);
  std::println("run-ms: {}", options.run_ms);
  std::println("max-packets: {}", options.max_packets);
  std::println("reply src-mac: {}", options.src_mac
                                        ? mac_string(*options.src_mac)
                                        : "virtio-net config MAC");
  std::println("match ethertype: {}",
               options.match_ethertype
                   ? std::format("0x{:04x}", *options.match_ethertype)
                   : std::string("any"));
  std::println("IRQ index: {} ({})", options.irq_index,
               vfio_irq_name(options.irq_index));
  std::println("RX MSI-X vector: {}", options.rx_vector);
  std::println("TX MSI-X vector: {}", options.tx_vector);
  std::println("dump-bytes: {}", options.dump_bytes);
  std::println("dump-every-packet: {}",
               options.dump_every_packet ? "yes" : "no");

  if (options.queue_size) {
    DmaLayout layout = compute_dma_layout(
        *options.queue_size, *options.queue_size, options.align,
        options.rx_buffers, options.tx_buffers, options.rx_buffer_size,
        options.rx_buffer_size);
    print_dma_layout(layout, options.iova, options.rx_buffers,
                     options.tx_buffers, options.rx_buffer_size);
  } else {
    std::println("queue_size: each target queue's device-reported size");
  }

  std::println("Would open VFIO container/group/device");
  std::println("Would validate IRQ index/vector eventfd support");
  std::println("Would bind RX/TX MSI-X vectors to eventfds");
  std::println("Would enable PCI Memory Space and Bus Master in CONFIG if "
               "needed");
  std::println("Would mmap virtio COMMON_CFG writable");
  std::println("Would mmap virtio DEVICE_CFG read-only");
  std::println("Would reset device and negotiate FEATURES_OK with "
               "VERSION_1/ACCESS_PLATFORM plus MAC/STATUS when offered");
  std::println("Would print virtio-net config MAC/status");
  std::println("Would inspect RX queue {} and TX queue {}", options.rx_queue,
               options.tx_queue);
  std::println("Would write queue_msix_vector for RX/TX queues");
  std::println("Would allocate and VFIO-map one DMA area for both vrings and "
               "RX/TX packet buffers");
  std::println("Would publish writable RX descriptors and set RX avail.idx");
  std::println("Would write queue_size, queue_desc, queue_avail, queue_used "
               "for both queues");
  std::println("Would enable both queues");
  std::println("Would write DRIVER_OK, notify RX, then use poll(eventfd) for "
               "up to {} ms or {} echoed packets",
               options.run_ms, options.max_packets);
  std::println("Would recycle RX descriptors after each consumed packet");
  std::println("Would reuse TX descriptors after TX used-ring completion");
  std::println("Would disable IRQ eventfds, reset the device, then unmap DMA");
  std::println("Would not implement a full network stack.");
  std::println("Pass --yes to perform these device and DMA writes.");
}

static void run_irq_echo_loop(const std::string &bdf, const fs::path &dev_dir,
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
  std::println("TX buffers: {} x {} bytes", options.tx_buffers,
               options.rx_buffer_size);
  std::println("run-ms: {}", options.run_ms);
  std::println("max-packets: {}", options.max_packets);
  std::println("reply src-mac: {}", options.src_mac
                                        ? mac_string(*options.src_mac)
                                        : "virtio-net config MAC");
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
  vfio_irq_info irq_info = validate_irq_vectors(vfio.device.get(), options);

  IrqEventfdBinding rx_irq(options.rx_vector);
  std::optional<IrqEventfdBinding> tx_irq;
  if (options.tx_vector != options.rx_vector) {
    tx_irq.emplace(options.tx_vector);
  } else {
    std::println("TX queue shares RX IRQ vector {}", options.rx_vector);
  }
  IrqIndexBindingGuard irq_bindings(vfio.device.get(), options.irq_index);
  bind_irq_eventfds(vfio.device.get(), options.irq_index, irq_info, rx_irq,
                    tx_irq ? &*tx_irq : nullptr);
  irq_bindings.activate();

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
  std::println("  TX buffer bytes: {}", tx_buffer_size);

  DmaLayout layout = compute_dma_layout(
      rx_queue_size, tx_queue_size, options.align, options.rx_buffers,
      options.tx_buffers, options.rx_buffer_size, tx_buffer_size);
  print_dma_layout(layout, options.iova, options.rx_buffers, options.tx_buffers,
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
  std::uint64_t rx_queue_iova = options.iova + layout.rx_vring_offset;
  std::uint64_t tx_queue_iova = options.iova + layout.tx_vring_offset;
  std::uint64_t rx_buffer_iova = options.iova + layout.rx_buffers_offset;
  std::uint64_t tx_buffers_iova = options.iova + layout.tx_buffers_offset;

  publish_rx_buffers(rx_queue_base, layout.rx_vring, rx_buffer_iova,
                     options.rx_buffers, options.rx_buffer_size);
  std::println("Published {} writable RX descriptors", options.rx_buffers);
  std::println("RX avail.idx: {}", static_cast<std::uint16_t>(*vring_avail_idx(
                                       rx_queue_base, layout.rx_vring)));
  std::println("Initial RX used.idx: {}",
               read_used_idx(rx_queue_base, layout.rx_vring));

  std::println("Initial TX used.idx: {}",
               read_used_idx(tx_queue_base, layout.tx_vring));

  QueueView rx_after_enable = program_and_enable_queue_with_msix(
      common_mapping, mapping_delta, selection, options.rx_queue,
      layout.rx_vring, rx_queue_iova, options.rx_vector, "RX queue");
  QueueView tx_after_enable = program_and_enable_queue_with_msix(
      common_mapping, mapping_delta, selection, options.tx_queue,
      layout.tx_vring, tx_queue_iova, options.tx_vector, "TX queue");

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

  std::vector<TxSlot> tx_slots(options.tx_buffers);
  std::uint16_t last_rx_used_idx =
      read_used_idx(rx_queue_base, layout.rx_vring);
  std::uint16_t last_tx_used_idx =
      read_used_idx(tx_queue_base, layout.tx_vring);
  std::uint32_t received_rx = 0;
  std::uint32_t echoed_tx = 0;
  std::uint32_t completed_tx = 0;
  std::uint32_t skipped_rx = 0;

  std::atomic_thread_fence(std::memory_order_release);
  set_driver_ok(common_mapping, mapping_delta);
  print_status("after DRIVER_OK", read_status(common_mapping, mapping_delta));

  write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
  std::println("Wrote one 16-bit RX notify value");
  std::println("Running IRQ-driven echo loop for up to {} ms or {} packets",
               options.run_ms, options.max_packets);

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(options.run_ms);
  IrqEventfdBinding *tx_irq_ptr = tx_irq ? &*tx_irq : nullptr;

  while (echoed_tx < options.max_packets &&
         std::chrono::steady_clock::now() < deadline) {
    bool made_progress =
        consume_tx_completions(tx_queue_base, layout.tx_vring, last_tx_used_idx,
                               tx_slots, completed_tx);

    std::uint16_t current_rx_used =
        read_used_idx(rx_queue_base, layout.rx_vring);
    std::uint16_t rx_count = used_delta(last_rx_used_idx, current_rx_used);

    for (std::uint16_t i = 0; i < rx_count; ++i) {
      std::uint16_t used_slot = static_cast<std::uint16_t>(
          (last_rx_used_idx + i) % layout.rx_vring.queue_size);
      std::optional<ReceivedPacket> received = read_rx_used_packet(
          buffer.data(), rx_queue_base, layout, used_slot, options.rx_buffers,
          options.rx_buffer_size, options.match_ethertype);
      made_progress = true;

      if (!received) {
        ++skipped_rx;
        const volatile VringUsedElem *elems =
            vring_used_elems(rx_queue_base, layout.rx_vring);
        std::uint32_t id = static_cast<std::uint32_t>(elems[used_slot].id);
        if (id < options.rx_buffers) {
          recycle_rx_buffer(rx_queue_base, layout.rx_vring,
                            static_cast<std::uint16_t>(id));
          write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
        }
        continue;
      }

      ++received_rx;
      consume_tx_completions(tx_queue_base, layout.tx_vring, last_tx_used_idx,
                             tx_slots, completed_tx);
      std::optional<std::uint16_t> tx_slot = find_free_tx_slot(tx_slots);
      if (!tx_slot) {
        std::println("No free TX descriptor; recycling RX descriptor {} "
                     "without echo",
                     received->desc_id);
        recycle_rx_buffer(rx_queue_base, layout.rx_vring,
                          static_cast<std::uint16_t>(received->desc_id));
        write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);
        continue;
      }

      const std::uint8_t *rx_ethernet = received->buffer + kVirtioNetHeaderSize;
      std::array<std::uint8_t, 6> reply_dst_mac =
          mac_from_bytes(rx_ethernet + 6);
      std::uint8_t *tx_packet =
          buffer.data() + layout.tx_buffers_offset +
          static_cast<std::size_t>(*tx_slot) * layout.tx_buffer_size;
      std::uint64_t tx_packet_iova =
          tx_buffers_iova +
          static_cast<std::uint64_t>(*tx_slot) * layout.tx_buffer_size;
      std::size_t tx_used_len = build_tx_reply_from_rx(
          tx_packet, layout.tx_buffer_size, received->buffer,
          received->used_len, reply_src_mac);

      publish_tx_packet(tx_queue_base, layout.tx_vring, *tx_slot,
                        tx_packet_iova, tx_used_len);
      tx_slots[*tx_slot].in_flight = true;
      ++echoed_tx;
      std::println("Echoed packet {}:", echoed_tx);
      std::println("  RX used slot: {}", received->used_slot);
      std::println("  RX desc id: {}", received->desc_id);
      std::println("  RX len: {}", received->used_len);
      std::println("  ethertype: 0x{:04x}", received->ethertype);
      std::println("  reply dst-mac: {}", mac_string(reply_dst_mac));
      std::println("  reply src-mac: {}", mac_string(reply_src_mac));
      std::println("  TX desc id: {}", *tx_slot);
      std::println("  TX bytes: {}", tx_used_len);
      if ((echoed_tx == 1 || options.dump_every_packet) &&
          options.dump_bytes > 0) {
        std::println("{} RX buffer dump:", options.dump_every_packet
                                               ? "Echoed packet"
                                               : "First echoed");
        print_virtio_net_rx_buffer(received->buffer, received->used_len,
                                   options.dump_bytes);
        std::println("{} TX reply packet prefix:",
                     options.dump_every_packet ? "Echoed packet" : "First");
        print_hex_dump(tx_packet, tx_used_len, options.dump_bytes);
      }

      write_notify(tx_notify_mapping, tx_notify_delta, tx_notify_value);
      recycle_rx_buffer(rx_queue_base, layout.rx_vring,
                        static_cast<std::uint16_t>(received->desc_id));
      write_notify(rx_notify_mapping, rx_notify_delta, rx_notify_value);

      if (echoed_tx >= options.max_packets)
        break;
    }

    last_rx_used_idx = current_rx_used;
    if (echoed_tx >= options.max_packets)
      break;
    if (made_progress)
      continue;

    int timeout_ms = timeout_until(deadline);
    if (timeout_ms <= 0)
      break;
    if (!poll_irq_events(rx_irq, tx_irq_ptr, timeout_ms))
      break;
  }

  auto tx_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (completed_tx < echoed_tx &&
         std::chrono::steady_clock::now() < tx_deadline) {
    if (consume_tx_completions(tx_queue_base, layout.tx_vring, last_tx_used_idx,
                               tx_slots, completed_tx)) {
      continue;
    }
    int timeout_ms = timeout_until(tx_deadline);
    if (timeout_ms <= 0)
      break;
    if (!poll_irq_events(rx_irq, tx_irq_ptr, timeout_ms))
      break;
  }

  std::println("IRQ echo loop summary:");
  std::println("  received matching RX packets: {}", received_rx);
  std::println("  skipped RX packets: {}", skipped_rx);
  std::println("  echoed TX packets: {}", echoed_tx);
  std::println("  completed TX packets: {}", completed_tx);
  std::println("  RX avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(rx_queue_base, layout.rx_vring)));
  std::println("  RX used.idx: {}",
               read_used_idx(rx_queue_base, layout.rx_vring));
  std::println("  TX avail.idx: {}",
               static_cast<std::uint16_t>(
                   *vring_avail_idx(tx_queue_base, layout.tx_vring)));
  std::println("  TX used.idx: {}",
               read_used_idx(tx_queue_base, layout.tx_vring));
  print_status("after loop", read_status(common_mapping, mapping_delta));

  irq_bindings.disable();

  selection.restore();
  std::println("Restored queue_select to {}", original_select);

  reset_device(common_mapping, mapping_delta, "after cleanup reset");
  reset_guard.disarm();
  pci_command.restore();

  std::uint64_t unmapped = dma.unmap();
  std::println("Unmapped IOVA 0x{:x}; kernel reported {} bytes unmapped",
               options.iova, unmapped);
  std::println("Stopped IRQ-driven echo loop and cleaned up the device.");
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
      } else if (arg == "--tx-buffers" && i + 1 < argc) {
        options.tx_buffers = parse_u16(argv[++i], "tx-buffers");
      } else if (arg == "--rx-buffer-size" && i + 1 < argc) {
        options.rx_buffer_size =
            static_cast<std::size_t>(parse_ull(argv[++i], "rx-buffer-size"));
      } else if (arg == "--iova" && i + 1 < argc) {
        options.iova = parse_ull(argv[++i], "iova");
      } else if (arg == "--align" && i + 1 < argc) {
        options.align = static_cast<std::size_t>(parse_ull(argv[++i], "align"));
      } else if (arg == "--run-ms" && i + 1 < argc) {
        options.run_ms = parse_u32(argv[++i], "run-ms");
      } else if (arg == "--max-packets" && i + 1 < argc) {
        options.max_packets = parse_u32(argv[++i], "max-packets");
      } else if (arg == "--src-mac" && i + 1 < argc) {
        options.src_mac = parse_mac(argv[++i], "src-mac");
      } else if (arg == "--match-ethertype" && i + 1 < argc) {
        options.match_ethertype = parse_u16(argv[++i], "match-ethertype");
      } else if (arg == "--accept-any-ethertype") {
        options.match_ethertype.reset();
      } else if (arg == "--irq-index" && i + 1 < argc) {
        options.irq_index = parse_u32(argv[++i], "irq-index");
      } else if (arg == "--rx-vector" && i + 1 < argc) {
        options.rx_vector = parse_u16(argv[++i], "rx-vector");
      } else if (arg == "--tx-vector" && i + 1 < argc) {
        options.tx_vector = parse_u16(argv[++i], "tx-vector");
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

    if (options.max_packets == 0)
      throw std::runtime_error("max-packets must be greater than 0");
    if (options.run_ms == 0)
      throw std::runtime_error("run-ms must be greater than 0");

    if (options.yes)
      run_irq_echo_loop(bdf, dev_dir, options);
    else
      dry_run(bdf, dev_dir, options);

    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
