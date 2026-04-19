// Exercise 02: PCI Config Space Parser
//
// Goal: read the raw 256-byte (or 4K extended) PCI configuration space
// and decode the header + capability list.
//
// Background
// ──────────
// Every PCI device has a standardized config space accessible at
// /sys/bus/pci/devices/<BDF>/config as a binary file.  The first
// 64 bytes are the "header" (standard across all PCI devices), the
// next 192 bytes are for vendor-specific use, and a PCIe device
// has a full 4KB extended config space.
//
// The header layout (Type 0, endpoint):
//   0x00  vendor_id (2)     0x02  device_id (2)
//   0x04  command   (2)     0x06  status    (2)
//   0x08  revision  (1)     0x09  class_code (3)
//   0x0C  cache_line (1)    0x0D  latency   (1)   0x0E  header_type (1)
//   0x10–0x27  BAR0..BAR5 (4 bytes each)
//   0x2C  subsystem_vendor  0x2E  subsystem_device
//   0x34  capabilities_pointer (1)
//   0x3C  interrupt_line    0x3D  interrupt_pin
//
// If status bit 4 is set, there's a capability list starting at
// config[cap_ptr].  Each capability is:
//   cap[0]  cap_id
//   cap[1]  next_ptr (0 = end of list)
//   cap[2..]  cap-specific data
//
// This is where you find MSI, MSI-X, PCI Express, Power Management,
// Vendor-Specific caps, etc.
//
// New concepts
// ────────────
// - PCI config space layout
// - Reading binary sysfs files
// - Capability list traversal
// - Common PCI capabilities (MSI, MSI-X, PCIe)

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// ─── Read the config file into memory ───────────────────────────────

static std::vector<uint8_t> read_config(const fs::path &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error(std::format("cannot open {}", path.string()));
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ─── Little-endian field reads ──────────────────────────────────────
//
// PCI config space is little-endian.  std::bit_cast would work if
// the layout were an aggregate, but for arbitrary offsets these
// helpers are clearer.

static uint16_t read16(std::span<const uint8_t> cfg, size_t off) {
  return uint16_t(cfg[off]) | (uint16_t(cfg[off + 1]) << 8);
}

static uint32_t read32(std::span<const uint8_t> cfg, size_t off) {
  return uint32_t(cfg[off]) | (uint32_t(cfg[off + 1]) << 8) |
         (uint32_t(cfg[off + 2]) << 16) | (uint32_t(cfg[off + 3]) << 24);
}

// ─── Capability ID → name ───────────────────────────────────────────

static std::string_view cap_name(uint8_t id) {
  static constexpr std::pair<uint8_t, std::string_view> table[] = {
      {0x01, "Power Management"},
      {0x03, "VPD"},
      {0x05, "MSI"},
      {0x07, "PCI-X"},
      {0x09, "Vendor Specific"},
      {0x0D, "Subsystem ID"},
      {0x10, "PCI Express"},
      {0x11, "MSI-X"},
      {0x12, "SATA Data/Idx"},
      {0x13, "Advanced Features"},
  };
  for (const auto &[code, name] : table)
    if (code == id)
      return name;
  return "Unknown";
}

// ─── Decode MSI-X capability (the interesting one) ──────────────────
//
// MSI-X layout (starting at cap offset):
//   +0  cap_id (0x11)    +1  next_ptr
//   +2  message_control  (table_size low 11 bits, enable bit 15)
//   +4  table_offset_bir (low 3 bits = BAR index, rest = offset)
//   +8  pba_offset_bir   (same)

static void decode_msix(std::span<const uint8_t> cfg, uint8_t off) {
  uint16_t ctrl = read16(cfg, off + 2);
  uint32_t tbl = read32(cfg, off + 4);
  uint32_t pba = read32(cfg, off + 8);

  unsigned table_size = (ctrl & 0x7FF) + 1;
  bool enabled = ctrl & 0x8000;
  bool masked = ctrl & 0x4000;
  unsigned tbl_bir = tbl & 0x7;
  uint32_t tbl_off = tbl & ~0x7U;
  unsigned pba_bir = pba & 0x7;
  uint32_t pba_off = pba & ~0x7U;

  std::println("      table_size={}  enable={}  mask={}", table_size, enabled,
               masked);
  std::println("      table at BAR{} + {:#x}", tbl_bir, tbl_off);
  std::println("      PBA   at BAR{} + {:#x}", pba_bir, pba_off);
}

// ─── Walk the capability list ───────────────────────────────────────

static void walk_caps(std::span<const uint8_t> cfg) {
  uint16_t status = read16(cfg, 0x06);
  if (!(status & (1 << 4))) {
    std::println("  (no capability list)");
    return;
  }

  uint8_t ptr = cfg[0x34] & 0xFC; // low 2 bits reserved
  while (ptr != 0 && ptr + 2 <= cfg.size()) {
    uint8_t id = cfg[ptr];
    uint8_t next = cfg[ptr + 1];
    std::println("  cap @ {:#04x}: id={:#04x} [{}]", ptr, id, cap_name(id));

    if (id == 0x11)
      decode_msix(cfg, ptr);

    if (next == 0 || next == ptr)
      break;
    ptr = next & 0xFC;
  }
}

// ─── Print one device's header + caps ───────────────────────────────

static void dump_device(const fs::path &dev_dir,
                        std::span<const uint8_t> s) {
  uint16_t vendor = read16(s, 0x00);
  uint16_t device = read16(s, 0x02);
  uint16_t command = read16(s, 0x04);
  uint16_t status = read16(s, 0x06);
  uint16_t subv = read16(s, 0x2C);
  uint16_t subd = read16(s, 0x2E);
  uint8_t irq_line = s[0x3C];
  uint8_t irq_pin = s[0x3D];

  std::println("{}  {:04x}:{:04x}  (subsys {:04x}:{:04x})",
               dev_dir.filename().string(), vendor, device, subv, subd);
  std::println("  command={:#06x}  status={:#06x}  IRQ line={} pin={}", command,
               status, irq_line, irq_pin);
  walk_caps(s);
  std::println("");
}

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  fs::path pci_root = "/sys/bus/pci/devices";

  // One-shot: dump a specific BDF.  Accept either full form
  // "DDDD:BB:DD.F" or short form "BB:DD.F" (domain 0000 assumed).
  if (argc == 2 && std::string_view(argv[1]).contains(':')) {
    std::string bdf = argv[1];
    // Short form has one colon before the first dot (BB:DD.F).
    // Full form has two (DDDD:BB:DD.F).
    if (std::count(bdf.begin(), bdf.end(), ':') == 1)
      bdf = "0000:" + bdf;
    auto cfg = read_config(pci_root / bdf / "config");
    dump_device(pci_root / bdf, cfg);
    return 0;
  }

  // Otherwise: --vendor / --class filters
  std::string vendor_filter;
  std::string class_filter;
  for (int i = 1; i < argc - 1; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--vendor")
      vendor_filter = argv[i + 1];
    else if (arg == "--class")
      class_filter = argv[i + 1];
  }

  std::vector<fs::path> devices;
  for (const auto &entry : fs::directory_iterator(pci_root))
    devices.push_back(entry.path());
  std::sort(devices.begin(), devices.end());

  for (const auto &dev : devices) {
    try {
      auto cfg = read_config(dev / "config");
      std::span<const uint8_t> s{cfg};

      if (!vendor_filter.empty() &&
          vendor_filter != std::format("{:04x}", read16(s, 0x00)))
        continue;
      // PCI header: class byte is at offset 0x0B
      if (!class_filter.empty() &&
          class_filter != std::format("{:02x}", cfg[0x0B]))
        continue;

      dump_device(dev, s);
    } catch (const std::exception &e) {
      std::println(std::cerr, "{}: {}", dev.filename().string(), e.what());
    }
  }
  return 0;
}
