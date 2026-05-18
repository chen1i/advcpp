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
//   /sys/bus/pci/devices/<BDF>/driver_override
//   /sys/bus/pci/drivers/<driver>/bind
//   /sys/bus/pci/drivers/<driver>/unbind
//
// Writing a BDF to unbind removes the device from its current driver.
// Writing a BDF to bind asks a driver to claim that device.  The bind
// operation still has to match the driver.  For vfio-pci, driver_override
// is the usual way to say "bind this exact device to vfio-pci".
// Rebind combines both steps: unbind the current driver if present, then
// bind the target driver.
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
// - driver_override for explicit driver matching
// - dry-run before dangerous hardware operations

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

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
               "  {} <BDF> --bind <driver> [--override] [--dry-run|--yes]\n"
               "  {} <BDF> --rebind <driver> [--override] [--dry-run|--yes]\n\n"
               "Examples:\n"
               "  {} c1:02.7 --show\n"
               "  {} c1:02.7 --unbind --dry-run\n"
               "  {} c1:02.7 --bind vfio-pci --override --dry-run\n"
               "  {} c1:02.7 --rebind vfio-pci --override --dry-run",
               argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
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

static std::string read_first_line(const fs::path &path) {
  std::ifstream in(path);
  if (!in)
    return {};

  std::string line;
  std::getline(in, line);
  return line;
}

static std::string current_driver_override(const fs::path &dev_dir) {
  fs::path override_file = dev_dir / "driver_override";
  if (!fs::exists(override_file))
    return "<unsupported>";

  std::string value = read_first_line(override_file);
  if (value.empty() || value == "(null)")
    return "<none>";
  return value;
}

static void show_device(const std::string &bdf, const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);

  std::println("BDF: {}", bdf);
  std::println("device path: {}", dev_dir.string());
  std::println("driver: {}", driver.name);
  if (driver.bound)
    std::println("driver path: {}", driver.path.string());
  std::println("driver_override: {}", current_driver_override(dev_dir));
}

static std::string errno_text(int error) {
  return std::strerror(error);
}

static void write_text(const fs::path &control_file, const std::string &value) {
  int fd = ::open(control_file.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd == -1) {
    int error = errno;
    throw std::runtime_error("cannot open " + control_file.string() + ": " +
                             errno_text(error));
  }

  ssize_t written = ::write(fd, value.data(), value.size());
  if (written == -1) {
    int error = errno;
    ::close(fd);
    throw std::runtime_error("cannot write to " + control_file.string() +
                             ": " + errno_text(error));
  }

  if (static_cast<std::size_t>(written) != value.size()) {
    ::close(fd);
    throw std::runtime_error("short write to " + control_file.string());
  }

  if (::close(fd) == -1) {
    int error = errno;
    throw std::runtime_error("cannot close " + control_file.string() + ": " +
                             errno_text(error));
  }
}

static void maybe_write_value(const fs::path &control_file,
                              const std::string &value, bool yes) {
  if (!yes) {
    std::println("Would write \"{}\" to {}", value, control_file.string());
    return;
  }

  write_text(control_file, value + "\n");
  std::println("Wrote \"{}\" to {}", value, control_file.string());
}

static void verify_unbound(const std::string &bdf, const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);
  if (driver.bound) {
    throw std::runtime_error(bdf + " is still bound to " + driver.name);
  }

  std::println("{} is now unbound", bdf);
}

static void verify_bound(const std::string &bdf, const fs::path &dev_dir,
                         const std::string &target_driver,
                         bool used_override) {
  DriverInfo driver = current_driver(dev_dir);
  if (driver.bound && driver.name == target_driver) {
    std::println("{} is now bound to {}", bdf, target_driver);
    return;
  }

  std::string current =
      driver.bound ? "bound to " + driver.name : std::string("unbound");
  std::string message =
      bdf + " is still " + current + " after bind attempt";
  if (!used_override) {
    message +=
        "; the driver may not match this PCI ID. For vfio-pci, retry with "
        "--override or register the ID with new_id.";
  } else {
    message += "; check dmesg for vfio/IOMMU/probe errors.";
  }
  throw std::runtime_error(message);
}

static void unbind_current(const std::string &bdf, const fs::path &dev_dir,
                           bool yes) {
  DriverInfo driver = current_driver(dev_dir);
  if (!driver.bound) {
    std::println("{} is already unbound", bdf);
    return;
  }

  maybe_write_value(driver.path / "unbind", bdf, yes);
  if (yes)
    verify_unbound(bdf, dev_dir);
}

static fs::path require_driver_dir(const std::string &target_driver) {
  fs::path driver_dir = fs::path("/sys/bus/pci/drivers") / target_driver;
  if (!fs::exists(driver_dir)) {
    throw std::runtime_error("driver " + target_driver + " does not exist");
  }
  return driver_dir;
}

static void bind_to_driver(const std::string &bdf, const fs::path &dev_dir,
                           const std::string &target_driver,
                           bool use_override, bool yes) {
  fs::path driver_dir = require_driver_dir(target_driver);

  if (use_override)
    maybe_write_value(dev_dir / "driver_override", target_driver, yes);
  maybe_write_value(driver_dir / "bind", bdf, yes);

  if (yes)
    verify_bound(bdf, dev_dir, target_driver, use_override);
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
  bool bind = false;
  bool rebind = false;
  bool yes = false;
  bool use_override = false;
  std::string target_driver;

  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--show") {
      show = true;
    } else if (arg == "--unbind") {
      unbind = true;
    } else if (arg == "--bind" && i + 1 < argc) {
      bind = true;
      target_driver = argv[++i];
    } else if (arg == "--rebind" && i + 1 < argc) {
      rebind = true;
      target_driver = argv[++i];
    } else if (arg == "--override") {
      use_override = true;
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
                static_cast<int>(bind) + static_cast<int>(rebind);
  if (actions != 1) {
    usage(argv[0]);
    return 1;
  }
  if (use_override && !bind && !rebind) {
    usage(argv[0]);
    return 1;
  }

  try {
    if (show) {
      show_device(bdf, dev_dir);
      return 0;
    }

    if (unbind) {
      unbind_current(bdf, dev_dir, yes);
      if (!yes)
        std::println("Pass --yes to perform this operation.");
      return 0;
    }

    if (rebind) {
      require_driver_dir(target_driver);
      unbind_current(bdf, dev_dir, yes);
      bind_to_driver(bdf, dev_dir, target_driver, use_override, yes);
      if (!yes)
        std::println("Pass --yes to perform this operation.");
      return 0;
    }

    if (bind) {
      bind_to_driver(bdf, dev_dir, target_driver, use_override, yes);
      if (!yes)
        std::println("Pass --yes to perform this operation.");
      return 0;
    }

    if (!yes)
      std::println("Pass --yes to perform this operation.");
    return 0;

  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
