// Exercise 04: PCI Driver Bind / Unbind
//
// Goal: inspect and safely change which Linux driver owns a PCI device.
//
// Background
// ──────────
// A PCI device can be bound to one kernel driver at a time.  Linux
// exposes this relationship in sysfs:
//
//   /sys/bus/pci/devices/<BDF>/driver  -> symlink to current driver
//   /sys/bus/pci/drivers/<driver>/bind
//   /sys/bus/pci/drivers/<driver>/unbind
//
// Writing a BDF to unbind removes the device from its current driver.
// Writing a BDF to bind asks a driver to claim that device.
//
// Safety note
// ───────────
// Bind/unbind changes real hardware ownership.  Unbinding a live NIC,
// storage controller, GPU, VFIO passthrough device, or DPDK device can
// break running workloads.  This sample defaults to dry-run behavior for
// bind/unbind.  Pass --yes only when you intentionally want the change.
//
// New concepts
// ────────────
// - PCI driver ownership
// - sysfs driver symlinks
// - bind/unbind control files
// - dry-run before dangerous hardware operations

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

static std::string normalize_bdf(std::string bdf) {
  if (std::count(bdf.begin(), bdf.end(), ':') == 1)
    bdf = "0000:" + bdf;
  return bdf;
}

static void usage(const char *argv0) {
  std::println(std::cerr,
               "Usage:\n"
               "  {} <BDF> --show\n"
               "  {} <BDF> --unbind [--dry-run|--yes]\n"
               "  {} <BDF> --bind <driver> [--dry-run|--yes]\n\n"
               "Examples:\n"
               "  {} c1:02.7 --show\n"
               "  {} c1:02.7 --unbind --dry-run\n"
               "  {} c1:02.7 --bind vfio-pci --dry-run",
               argv0, argv0, argv0, argv0, argv0, argv0);
}

struct DriverInfo {
  bool bound = false;
  std::string name = "<unbound>";
  fs::path path;
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

static void show_device(const std::string &bdf, const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);

  std::println("BDF: {}", bdf);
  std::println("device path: {}", dev_dir.string());
  std::println("driver: {}", driver.name);
  if (driver.bound)
    std::println("driver path: {}", driver.path.string());
}

static void write_bdf(const fs::path &control_file, const std::string &bdf) {
  std::ofstream out(control_file);
  if (!out)
    throw std::runtime_error("cannot open " + control_file.string());

  out << bdf;
  if (!out)
    throw std::runtime_error("cannot write " + bdf + " to " +
                             control_file.string());
}

static void maybe_write_bdf(const fs::path &control_file, const std::string &bdf,
                            bool yes) {
  if (!yes) {
    std::println("Would write \"{}\" to {}", bdf, control_file.string());
    std::println("Pass --yes to perform this operation.");
    return;
  }

  write_bdf(control_file, bdf);
  std::println("Wrote \"{}\" to {}", bdf, control_file.string());
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  std::string bdf = normalize_bdf(argv[1]);
  fs::path pci_root = "/sys/bus/pci/devices";
  fs::path dev_dir = pci_root / bdf;

  if (!fs::exists(dev_dir)) {
    std::println(std::cerr, "Error: PCI device {} does not exist", bdf);
    return 1;
  }

  bool show = false;
  bool unbind = false;
  bool yes = false;
  std::string bind_driver;

  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--show") {
      show = true;
    } else if (arg == "--unbind") {
      unbind = true;
    } else if (arg == "--bind" && i + 1 < argc) {
      bind_driver = argv[++i];
    } else if (arg == "--dry-run") {
      yes = false;
    } else if (arg == "--yes") {
      yes = true;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  int actions = static_cast<int>(show) + static_cast<int>(unbind) +
                static_cast<int>(!bind_driver.empty());
  if (actions != 1) {
    usage(argv[0]);
    return 1;
  }

  try {
    if (show) {
      show_device(bdf, dev_dir);
      return 0;
    }

    if (unbind) {
      DriverInfo driver = current_driver(dev_dir);
      if (!driver.bound) {
        std::println("{} is already unbound", bdf);
        return 0;
      }
      maybe_write_bdf(driver.path / "unbind", bdf, yes);
      return 0;
    }

    fs::path driver_dir = fs::path("/sys/bus/pci/drivers") / bind_driver;
    if (!fs::exists(driver_dir)) {
      std::println(std::cerr, "Error: driver {} does not exist", bind_driver);
      return 1;
    }

    maybe_write_bdf(driver_dir / "bind", bdf, yes);
    return 0;

  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
