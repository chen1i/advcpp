// Exercise 03: mmap a BAR and Read Device Registers
//
// Goal: map a PCI device's memory BAR into our address space and
// read registers directly.  This is what every userspace driver
// does — the kernel never sees our reads/writes once the BAR is
// mapped.
//
// Background
// ──────────
// Sysfs exposes each BAR at /sys/bus/pci/devices/<BDF>/resourceN
// as a file whose size equals the BAR size and whose contents are
// the device's registers.  mmap() on that file gives us a pointer
// into the BAR.
//
// Memory BAR vs I/O BAR
// ─────────────────────
// Only *memory* BARs can be mmap'd.  I/O BARs (bit 0 of the flags
// set) need port I/O instructions (inb/outl) and require ioperm()
// or CAP_SYS_RAWIO.  Modern PCIe devices almost always use memory
// BARs — I/O BARs are a legacy x86 thing.
//
// Why `volatile`?
// ───────────────
// Device registers can change between reads (the device modifies
// them asynchronously).  Without `volatile`, the compiler might
// cache the first read in a register and reuse it — disastrous
// when reading a "data available" status register.  Every MMIO
// access must go through `volatile`.
//
// Security note
// ─────────────
// mmap'ing a BAR that a kernel driver is currently using is risky
// for writes — you can corrupt the device state.  Reading is
// generally safe but might race with the driver.  For real driver
// development, we unbind the kernel driver first (next exercise).
//
// New concepts
// ────────────
// - mmap() with MAP_SHARED for device memory
// - `volatile` for MMIO access
// - BAR size via stat()
// - The resource file vs resource_wc file (write-combining)
// - PROT_READ vs PROT_READ | PROT_WRITE

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

// ─── Parse one BAR line from the device's resource table ────────────

struct BarResource {
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t flags = 0;

  uint64_t size() const { return end >= start ? end - start + 1 : 0; }
  bool is_io() const { return flags & 0x01; }
};

static BarResource read_bar_resource(const fs::path &dev_dir, int bar) {
  if (bar < 0 || bar > 5)
    throw std::invalid_argument("BAR index must be 0..5");

  std::ifstream f(dev_dir / "resource");
  if (!f)
    throw std::runtime_error(std::format("cannot open {}", (dev_dir / "resource").string()));

  std::string line;
  for (int i = 0; i <= bar; ++i) {
    if (!std::getline(f, line))
      throw std::runtime_error(std::format("missing BAR{} in resource table", bar));
  }

  BarResource r{};
  std::istringstream iss(line);
  iss >> std::hex >> r.start >> r.end >> r.flags;
  if (!iss)
    throw std::runtime_error(std::format("cannot parse BAR{} resource line", bar));
  return r;
}

// ─── RAII wrapper for mmap'd region ─────────────────────────────────

class MmioRegion {
public:
  MmioRegion(const fs::path &resource_file, size_t size, bool writable = false)
      : size_(size) {
    if (size_ == 0)
      throw std::invalid_argument("cannot mmap a zero-sized BAR");

    int flags = writable ? O_RDWR : O_RDONLY;
    fd_ = open(resource_file.c_str(), flags);
    if (fd_ < 0)
      throw std::system_error(errno, std::system_category(),
                              "open " + resource_file.string());

    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *p = mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
      close(fd_);
      throw std::system_error(errno, std::system_category(), "mmap");
    }
    base_ = static_cast<volatile uint8_t *>(p);
  }

  ~MmioRegion() {
    if (base_)
      munmap(const_cast<uint8_t *>(base_), size_);
    if (fd_ >= 0)
      close(fd_);
  }

  MmioRegion(const MmioRegion &) = delete;
  MmioRegion &operator=(const MmioRegion &) = delete;

  size_t size() const { return size_; }

  // Typed register access — `volatile` ensures each call really
  // hits the device, no caching, no reordering across sequence points.
  uint8_t read8(size_t off) const { return at<uint8_t>(off); }
  uint16_t read16(size_t off) const { return at<uint16_t>(off); }
  uint32_t read32(size_t off) const { return at<uint32_t>(off); }
  uint64_t read64(size_t off) const { return at<uint64_t>(off); }

  void write32(size_t off, uint32_t val) {
    *reinterpret_cast<volatile uint32_t *>(base_ + off) = val;
  }

private:
  template <typename T> T at(size_t off) const {
    if (off + sizeof(T) > size_)
      throw std::out_of_range("MMIO read past end of BAR");
    return *reinterpret_cast<const volatile T *>(base_ + off);
  }

  int fd_ = -1;
  volatile uint8_t *base_ = nullptr;
  size_t size_ = 0;
};

// ─── Hex dump a region ──────────────────────────────────────────────

static void hexdump(const MmioRegion &r, size_t offset, size_t len) {
  for (size_t i = 0; i < len; i += 16) {
    std::print("{:08x} ", offset + i);

    // Hex bytes
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < len)
        std::print(" {:02x}", r.read8(offset + i + j));
      else
        std::print("   ");
    }

    // ASCII
    std::print("  ");
    for (size_t j = 0; j < 16 && i + j < len; ++j) {
      uint8_t c = r.read8(offset + i + j);
      std::print("{}", (c >= 0x20 && c < 0x7F) ? char(c) : '.');
    }
    std::println("");
  }
}

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::println(std::cerr,
                 "Usage: {} <BDF> <bar_index> [offset] [length]\n"
                 "  BDF: 0000:06:12.0 or 06:12.0",
                 argv[0]);
    return 1;
  }

  std::string bdf = argv[1];
  if (std::count(bdf.begin(), bdf.end(), ':') == 1)
    bdf = "0000:" + bdf;

  int bar = std::stoi(argv[2]);
  size_t offset = (argc > 3) ? std::stoul(argv[3], nullptr, 0) : 0;
  size_t length = (argc > 4) ? std::stoul(argv[4], nullptr, 0) : 128;

  fs::path resource =
      fs::path("/sys/bus/pci/devices") / bdf / std::format("resource{}", bar);
  fs::path dev_dir = fs::path("/sys/bus/pci/devices") / bdf;

  try {
    BarResource bar_resource = read_bar_resource(dev_dir, bar);
    if (bar_resource.size() == 0)
      throw std::runtime_error(std::format("BAR{} is not present", bar));
    if (bar_resource.is_io())
      throw std::runtime_error(std::format("BAR{} is an I/O BAR; only memory BARs can be mmap'd",
                                           bar));
    if (offset >= bar_resource.size())
      throw std::out_of_range(std::format("offset {:#x} is past BAR{} size {:#x}",
                                          offset, bar, bar_resource.size()));

    MmioRegion r(resource, static_cast<size_t>(bar_resource.size()));
    std::println("Mapped {} ({} bytes)", resource.string(), r.size());
    std::println("Dumping {} bytes at offset {:#x}:", length, offset);
    hexdump(r, offset, std::min(length, r.size() - offset));
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }

  return 0;
}
