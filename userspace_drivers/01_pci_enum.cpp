// Exercise 01: PCI Device Enumeration
//
// Goal: discover PCI devices on the system and print their vital
// info — vendor, device, class, BARs — all from userspace.
//
// Background
// ──────────
// Linux exposes every PCI device at /sys/bus/pci/devices/<BDF>/
// where BDF = Bus:Device.Function (e.g. "0000:06:12.0").
//
// Each device has sysfs files we can read directly:
//   vendor, device         — 16-bit IDs (hex)
//   class                  — class code (device type)
//   resource               — BAR addresses and sizes
//   config                 — raw 256-byte (or 4K) PCI config space
//   driver                 — symlink to bound kernel driver, if any
//
// No root needed for most of these.  This exercise is the
// foundation for everything else — before you can drive a device,
// you need to find it.
//
// New concepts
// ────────────
// - PCI BDF addressing (Bus:Device.Function)
// - Vendor/Device IDs (e.g. Red Hat = 0x1af4 for virtio)
// - Class code hierarchy (class:subclass:prog-if)
// - BAR (Base Address Register) — memory/IO regions
// - sysfs as the userspace interface to kernel device info

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ─── Parse a hex line from a sysfs file ─────────────────────────────

static uint32_t read_hex(const fs::path &file) {
  std::ifstream f(file);
  std::string s;
  if (!std::getline(f, s))
    return 0;
  return std::stoul(s, nullptr, 16);
}

// ─── Parse a BAR resource line ──────────────────────────────────────
//
// Each line in /sys/bus/pci/devices/<BDF>/resource is:
//   <start> <end> <flags>
// All hex.  flags bit 0 = I/O (vs memory), bit 8 = prefetchable, etc.

struct Bar {
  uint64_t start;
  uint64_t end;
  uint64_t flags;

  uint64_t size() const { return end ? end - start + 1 : 0; }
  bool is_io() const { return flags & 0x01; }
  bool is_64bit() const { return (flags >> 1) & 0x03; }
  bool is_prefetchable() const { return flags & 0x08; }
};

static std::vector<Bar> read_bars(const fs::path &resource_file) {
  std::vector<Bar> bars;
  std::ifstream f(resource_file);
  std::string line;
  // PCI has up to 6 BARs (+ ROM); we only care about the 6 BARs.
  for (int i = 0; i < 6 && std::getline(f, line); ++i) {
    Bar b{};
    sscanf(line.c_str(), "%lx %lx %lx", &b.start, &b.end, &b.flags);
    bars.push_back(b);
  }
  return bars;
}

// ─── Class code → human name ────────────────────────────────────────

static const char *class_name(uint32_t class_code) {
  // class_code is 24-bit: (class << 16) | (subclass << 8) | prog_if
  uint8_t cls = (class_code >> 16) & 0xFF;
  switch (cls) {
  case 0x00:
    return "Unclassified";
  case 0x01:
    return "Mass Storage";
  case 0x02:
    return "Network";
  case 0x03:
    return "Display";
  case 0x04:
    return "Multimedia";
  case 0x05:
    return "Memory";
  case 0x06:
    return "Bridge";
  case 0x07:
    return "Communication";
  case 0x08:
    return "System";
  case 0x09:
    return "Input";
  case 0x0C:
    return "Serial Bus";
  case 0x0D:
    return "Wireless";
  default:
    return "Other";
  }
}

// ─── Print one device ───────────────────────────────────────────────

static void print_device(const fs::path &dev_dir) {
  std::string bdf = dev_dir.filename().string();
  uint32_t vendor = read_hex(dev_dir / "vendor");
  uint32_t device = read_hex(dev_dir / "device");
  uint32_t cls = read_hex(dev_dir / "class");

  // Driver binding (optional)
  std::string driver = "<unbound>";
  auto drv_link = dev_dir / "driver";
  if (fs::exists(drv_link) && fs::is_symlink(drv_link)) {
    driver = fs::read_symlink(drv_link).filename().string();
  }

  std::println("{}  {:04x}:{:04x}  [{}]  driver={}", bdf, vendor, device,
               class_name(cls), driver);

  // Print non-empty BARs
  auto bars = read_bars(dev_dir / "resource");
  for (size_t i = 0; i < bars.size(); ++i) {
    if (bars[i].size() == 0)
      continue;
    std::println("    BAR{}: {:#x}  size={}  {}{}{}", i, bars[i].start,
                 bars[i].size(), bars[i].is_io() ? "I/O" : "mem",
                 bars[i].is_prefetchable() ? " prefetch" : "",
                 bars[i].is_64bit() ? " 64-bit" : "");
  }
}

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  fs::path pci_root = "/sys/bus/pci/devices";

  // Optional filter: --vendor 1af4 to show only virtio
  std::string vendor_filter;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--vendor") {
      vendor_filter = argv[i + 1];
    }
  }

  std::vector<fs::path> devices;
  for (const auto &entry : fs::directory_iterator(pci_root)) {
    devices.push_back(entry.path());
  }
  std::sort(devices.begin(), devices.end());

  for (const auto &dev : devices) {
    if (!vendor_filter.empty()) {
      uint32_t v = read_hex(dev / "vendor");
      char buf[8];
      snprintf(buf, sizeof(buf), "%04x", v);
      if (vendor_filter != buf)
        continue;
    }
    print_device(dev);
  }

  return 0;
}
