// Exercise 05: PCI SR-IOV VF Control
//
// Goal: inspect and safely control SR-IOV VFs from userspace.
//
// Background
// ----------
// A PF (Physical Function) that supports SR-IOV exposes these sysfs files:
//
//   /sys/bus/pci/devices/<PF_BDF>/sriov_totalvfs
//   /sys/bus/pci/devices/<PF_BDF>/sriov_numvfs
//   /sys/bus/pci/devices/<PF_BDF>/sriov_drivers_autoprobe
//   /sys/bus/pci/devices/<PF_BDF>/virtfn<N> -> VF device symlink
//
// sriov_drivers_autoprobe controls whether newly created VFs are
// automatically probed and bound to matching kernel drivers.  Set it before
// writing sriov_numvfs.
//
// VF creation is controlled by the PF driver.  Keep the PF bound to its
// vendor driver while creating VFs; bind the VFs to vfio-pci afterwards if
// that is your target.
//
// Gotcha: virtio-net PFs need the virtio_net driver loaded before creating
// VFs.  The virtio-pci SR-IOV path returns EBUSY until the virtio child
// device reaches DRIVER_OK, so sriov_numvfs can fail even when it is 0 and no
// virtfn<N> links exist yet.
//
// Safety note
// -----------
// Creating or destroying VFs changes real host hardware state.  Destroying
// VFs can break VMs, DPDK processes, VFIO users, or network configuration.
// This sample defaults to dry-run behavior.  Pass --yes only when you
// intentionally want the change.
//
// New concepts
// ------------
// - SR-IOV PF and VF relationship
// - sriov_totalvfs and sriov_numvfs
// - sriov_drivers_autoprobe
// - virtfn<N> symlinks

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static std::string normalize_bdf(std::string bdf) {
  if (std::count(bdf.begin(), bdf.end(), ':') == 1)
    bdf = "0000:" + bdf;
  return bdf;
}

static void usage(const char *argv0) {
  std::println(std::cerr,
               "Usage:\n"
               "  {} <PF_BDF> --show\n"
               "  {} <PF_BDF> --set-autoprobe <0|1> [--dry-run|--yes]\n"
               "  {} <PF_BDF> --create <N> [--dry-run|--yes]\n"
               "  {} <PF_BDF> --destroy [--dry-run|--yes]\n\n"
               "Examples:\n"
               "  {} c1:00.0 --show\n"
               "  {} c1:00.0 --set-autoprobe 0 --dry-run\n"
               "  {} c1:00.0 --create 4 --dry-run\n"
               "  {} c1:00.0 --destroy --dry-run",
               argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static std::string errno_text(int error) {
  return std::strerror(error);
}

static std::string read_first_line(const fs::path &path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open " + path.string());

  std::string line;
  std::getline(in, line);
  if (!in && !in.eof())
    throw std::runtime_error("cannot read " + path.string());
  return line;
}

static unsigned parse_unsigned(std::string_view text, std::string_view name) {
  unsigned long long value = 0;
  const char *first = text.data();
  const char *last = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last ||
      value > std::numeric_limits<unsigned>::max()) {
    throw std::runtime_error("invalid " + std::string(name) + ": " +
                             std::string(text));
  }
  return static_cast<unsigned>(value);
}

static unsigned read_unsigned(const fs::path &path) {
  return parse_unsigned(read_first_line(path), path.filename().string());
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

static std::string current_driver_override(const fs::path &dev_dir) {
  fs::path override_file = dev_dir / "driver_override";
  if (!fs::exists(override_file))
    return "<unsupported>";

  std::string value = read_first_line(override_file);
  if (value.empty() || value == "(null)")
    return "<none>";
  return value;
}

struct VfInfo {
  unsigned index = 0;
  std::string bdf;
  fs::path path;
  DriverInfo driver;
  std::string driver_override;
};

static bool parse_virtfn_index(std::string_view name, unsigned &index) {
  constexpr std::string_view prefix = "virtfn";
  if (!name.starts_with(prefix))
    return false;

  std::string_view suffix = name.substr(prefix.size());
  if (suffix.empty())
    return false;

  try {
    index = parse_unsigned(suffix, "virtfn index");
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

static std::vector<VfInfo> list_vfs(const fs::path &pf_dir) {
  std::vector<VfInfo> vfs;

  for (const fs::directory_entry &entry : fs::directory_iterator(pf_dir)) {
    unsigned index = 0;
    if (!parse_virtfn_index(entry.path().filename().string(), index))
      continue;

    fs::path target = fs::read_symlink(entry.path());
    fs::path vf_path = target.is_absolute()
                           ? target
                           : fs::weakly_canonical(entry.path().parent_path() /
                                                  target);

    VfInfo vf;
    vf.index = index;
    vf.bdf = vf_path.filename().string();
    vf.path = vf_path;
    vf.driver = current_driver(vf_path);
    vf.driver_override = current_driver_override(vf_path);
    vfs.push_back(std::move(vf));
  }

  std::sort(vfs.begin(), vfs.end(),
            [](const VfInfo &a, const VfInfo &b) { return a.index < b.index; });
  return vfs;
}

struct SriovInfo {
  unsigned total_vfs = 0;
  unsigned num_vfs = 0;
  bool autoprobe_supported = false;
  unsigned autoprobe = 0;
};

static SriovInfo read_sriov_info(const fs::path &pf_dir) {
  fs::path total_file = pf_dir / "sriov_totalvfs";
  fs::path num_file = pf_dir / "sriov_numvfs";
  if (!fs::exists(total_file) || !fs::exists(num_file)) {
    throw std::runtime_error(pf_dir.string() +
                             " does not expose SR-IOV controls");
  }

  SriovInfo info;
  info.total_vfs = read_unsigned(total_file);
  info.num_vfs = read_unsigned(num_file);

  fs::path autoprobe_file = pf_dir / "sriov_drivers_autoprobe";
  if (fs::exists(autoprobe_file)) {
    info.autoprobe_supported = true;
    info.autoprobe = read_unsigned(autoprobe_file);
  }

  return info;
}

static void show_pf(const std::string &bdf, const fs::path &pf_dir) {
  SriovInfo info = read_sriov_info(pf_dir);
  DriverInfo pf_driver = current_driver(pf_dir);

  std::println("PF BDF: {}", bdf);
  std::println("device path: {}", pf_dir.string());
  std::println("PF driver: {}", pf_driver.name);
  if (pf_driver.bound)
    std::println("PF driver path: {}", pf_driver.path.string());
  std::println("PF driver_override: {}", current_driver_override(pf_dir));
  std::println("sriov_totalvfs: {}", info.total_vfs);
  std::println("sriov_numvfs: {}", info.num_vfs);
  if (info.autoprobe_supported)
    std::println("sriov_drivers_autoprobe: {}", info.autoprobe);
  else
    std::println("sriov_drivers_autoprobe: <unsupported>");

  std::vector<VfInfo> vfs = list_vfs(pf_dir);
  if (vfs.empty()) {
    std::println("VFs: <none>");
    return;
  }

  std::println("VFs:");
  for (const VfInfo &vf : vfs) {
    std::println("  virtfn{}: {}  driver={}  driver_override={}", vf.index,
                 vf.bdf, vf.driver.name, vf.driver_override);
  }
}

static void require_no_vfs(const SriovInfo &info, std::string_view operation) {
  if (info.num_vfs != 0) {
    throw std::runtime_error(std::string(operation) +
                             " requires sriov_numvfs=0; destroy VFs first");
  }
}

static void require_pf_driver_for_vf_creation(const fs::path &pf_dir) {
  DriverInfo driver = current_driver(pf_dir);
  if (!driver.bound) {
    throw std::runtime_error(
        "--create requires the PF vendor driver; PF is unbound");
  }

  if (driver.name == "vfio-pci") {
    throw std::runtime_error(
        "--create requires the PF vendor driver; PF is bound to vfio-pci. "
        "Create VFs with the PF vendor driver first, then bind VFs to "
        "vfio-pci");
  }
}

static void set_autoprobe(const fs::path &pf_dir, unsigned value, bool yes) {
  if (value > 1)
    throw std::runtime_error("--set-autoprobe expects 0 or 1");

  fs::path autoprobe_file = pf_dir / "sriov_drivers_autoprobe";
  if (!fs::exists(autoprobe_file)) {
    throw std::runtime_error("sriov_drivers_autoprobe is not supported");
  }

  SriovInfo info = read_sriov_info(pf_dir);
  require_no_vfs(info, "--set-autoprobe");

  maybe_write_value(autoprobe_file, std::to_string(value), yes);
  if (yes) {
    unsigned actual = read_unsigned(autoprobe_file);
    if (actual != value) {
      throw std::runtime_error("sriov_drivers_autoprobe did not change to " +
                               std::to_string(value));
    }
    std::println("sriov_drivers_autoprobe is now {}", actual);
  }
}

static void create_vfs(const fs::path &pf_dir, unsigned requested, bool yes) {
  SriovInfo info = read_sriov_info(pf_dir);
  require_no_vfs(info, "--create");

  if (requested == 0)
    throw std::runtime_error("--create expects N > 0; use --destroy for 0");
  if (requested > info.total_vfs) {
    throw std::runtime_error("requested " + std::to_string(requested) +
                             " VFs, but sriov_totalvfs is " +
                             std::to_string(info.total_vfs));
  }
  require_pf_driver_for_vf_creation(pf_dir);

  if (info.autoprobe_supported)
    std::println("sriov_drivers_autoprobe is currently {}", info.autoprobe);

  maybe_write_value(pf_dir / "sriov_numvfs", std::to_string(requested), yes);
  if (yes) {
    unsigned actual = read_unsigned(pf_dir / "sriov_numvfs");
    if (actual != requested) {
      throw std::runtime_error("sriov_numvfs did not change to " +
                               std::to_string(requested));
    }
    std::println("sriov_numvfs is now {}", actual);
  }
}

static void destroy_vfs(const fs::path &pf_dir, bool yes) {
  SriovInfo info = read_sriov_info(pf_dir);
  if (info.num_vfs == 0) {
    std::println("sriov_numvfs is already 0");
    return;
  }

  maybe_write_value(pf_dir / "sriov_numvfs", "0", yes);
  if (yes) {
    unsigned actual = read_unsigned(pf_dir / "sriov_numvfs");
    if (actual != 0)
      throw std::runtime_error("sriov_numvfs did not change to 0");
    std::println("sriov_numvfs is now 0");
  }
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  std::string bdf = normalize_bdf(argv[1]);
  fs::path pci_root = "/sys/bus/pci/devices";
  fs::path pf_dir = pci_root / bdf;

  if (!fs::exists(pf_dir)) {
    std::println(std::cerr, "Error: PCI device {} does not exist", bdf);
    return 1;
  }

  bool show = false;
  bool set_autoprobe_requested = false;
  bool create_requested = false;
  bool destroy_requested = false;
  bool yes = false;
  unsigned autoprobe_value = 0;
  unsigned vf_count = 0;

  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--show") {
      show = true;
    } else if (arg == "--set-autoprobe" && i + 1 < argc) {
      set_autoprobe_requested = true;
      autoprobe_value = parse_unsigned(argv[++i], "--set-autoprobe");
    } else if (arg == "--create" && i + 1 < argc) {
      create_requested = true;
      vf_count = parse_unsigned(argv[++i], "--create");
    } else if (arg == "--destroy") {
      destroy_requested = true;
    } else if (arg == "--dry-run") {
      yes = false;
    } else if (arg == "--yes") {
      yes = true;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  int actions = static_cast<int>(show) +
                static_cast<int>(set_autoprobe_requested) +
                static_cast<int>(create_requested) +
                static_cast<int>(destroy_requested);
  if (actions != 1) {
    usage(argv[0]);
    return 1;
  }

  try {
    if (show) {
      show_pf(bdf, pf_dir);
      return 0;
    }

    if (set_autoprobe_requested) {
      set_autoprobe(pf_dir, autoprobe_value, yes);
    } else if (create_requested) {
      create_vfs(pf_dir, vf_count, yes);
    } else if (destroy_requested) {
      destroy_vfs(pf_dir, yes);
    }

    if (!yes)
      std::println("Pass --yes to perform this operation.");
    return 0;

  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
