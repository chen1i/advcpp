// Exercise 10: Virtio PCI capabilities through VFIO
//
// Goal: discover where a modern virtio PCI device exposes its configuration
// structures.
//
// Background
// ----------
// A modern virtio PCI device publishes vendor capabilities in PCI config
// space.  Those capabilities point at BAR ranges for:
//
//   common_cfg, notify_cfg, isr_cfg, device_cfg, pci_cfg, and optional shm_cfg
//
// Before a userspace driver can configure queues, notify the device, or read
// device-specific config, it needs this map.  This sample reads PCI config
// space through the VFIO PCI CONFIG region and parses the virtio capabilities.
// It does not mmap BARs, write registers, reset the device, or start DMA.
//
// New concepts
// ------------
// - VFIO PCI CONFIG region
// - PCI capability list traversal
// - Modern virtio PCI vendor capabilities
// - common_cfg / notify_cfg / isr_cfg / device_cfg discovery

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <linux/pci_regs.h>
#include <linux/vfio.h>
#include <linux/virtio_pci.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }

  ~UniqueFd() { reset(); }

  int get() const { return fd_; }

  int release() {
    int old = fd_;
    fd_ = -1;
    return old;
  }

  void reset(int fd = -1) {
    if (fd_ != -1)
      ::close(fd_);
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

struct DriverInfo {
  bool bound = false;
  std::string name = "<unbound>";
  fs::path path;
};

struct IommuGroupInfo {
  std::string id;
  fs::path path;
};

struct VfioContext {
  UniqueFd container;
  UniqueFd group;
  UniqueFd device;
};

struct ConfigRegion {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t flags = 0;
};

static std::string errno_text(int error) {
  return std::strerror(error);
}

static std::string normalize_bdf(std::string bdf) {
  if (std::count(bdf.begin(), bdf.end(), ':') == 1)
    bdf = "0000:" + bdf;
  return bdf;
}

static void usage(const char *argv0) {
  std::println(std::cerr,
               "Usage:\n"
               "  {} <BDF> --show\n\n"
               "Example:\n"
               "  {} c1:00.6 --show",
               argv0, argv0);
}

static std::string join_flags(const std::vector<std::string_view> &flags) {
  if (flags.empty())
    return "none";

  std::string out;
  for (std::string_view flag : flags) {
    if (!out.empty())
      out += "|";
    out += flag;
  }
  return out;
}

static std::string region_flag_names(__u32 flags) {
  std::vector<std::string_view> names;
  if (flags & VFIO_REGION_INFO_FLAG_READ)
    names.push_back("READ");
  if (flags & VFIO_REGION_INFO_FLAG_WRITE)
    names.push_back("WRITE");
  if (flags & VFIO_REGION_INFO_FLAG_MMAP)
    names.push_back("MMAP");
  if (flags & VFIO_REGION_INFO_FLAG_CAPS)
    names.push_back("CAPS");
  return join_flags(names);
}

static std::string virtio_cfg_type_name(std::uint8_t cfg_type) {
  switch (cfg_type) {
  case VIRTIO_PCI_CAP_COMMON_CFG:
    return "COMMON_CFG";
  case VIRTIO_PCI_CAP_NOTIFY_CFG:
    return "NOTIFY_CFG";
  case VIRTIO_PCI_CAP_ISR_CFG:
    return "ISR_CFG";
  case VIRTIO_PCI_CAP_DEVICE_CFG:
    return "DEVICE_CFG";
  case VIRTIO_PCI_CAP_PCI_CFG:
    return "PCI_CFG";
  case VIRTIO_PCI_CAP_SHARED_MEMORY_CFG:
    return "SHM_CFG";
  case VIRTIO_PCI_CAP_VENDOR_CFG:
    return "VENDOR_CFG";
  default:
    return "unknown";
  }
}

static UniqueFd open_fd(const fs::path &path, int flags) {
  int fd = ::open(path.c_str(), flags | O_CLOEXEC);
  if (fd == -1) {
    int error = errno;
    std::string message = "cannot open " + path.string() + ": " +
                          errno_text(error);
    if (error == EBUSY && path.string().starts_with("/dev/vfio/")) {
      message +=
          "; the VFIO group is already open in another process. Check with "
          "fuser or lsof and stop the process that owns this group";
    }
    throw std::runtime_error(message);
  }
  return UniqueFd(fd);
}

static void ioctl_checked(int fd, unsigned long request, void *arg,
                          std::string_view operation) {
  if (::ioctl(fd, request, arg) == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             errno_text(error));
  }
}

static int ioctl_value(int fd, unsigned long request,
                       std::string_view operation) {
  int result = ::ioctl(fd, request);
  if (result == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             errno_text(error));
  }
  return result;
}

static int ioctl_arg_value(int fd, unsigned long request, unsigned long arg,
                           std::string_view operation) {
  int result = ::ioctl(fd, request, arg);
  if (result == -1) {
    int error = errno;
    throw std::runtime_error(std::string(operation) + ": " +
                             errno_text(error));
  }
  return result;
}

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

static void require_vfio_driver(const fs::path &dev_dir) {
  DriverInfo driver = current_driver(dev_dir);
  if (driver.name != "vfio-pci") {
    throw std::runtime_error("device must be bound to vfio-pci; current driver=" +
                             driver.name);
  }
}

static IommuGroupInfo iommu_group_for_device(const fs::path &dev_dir) {
  fs::path group_link = dev_dir / "iommu_group";
  if (!fs::exists(group_link)) {
    throw std::runtime_error(dev_dir.string() +
                             " does not have an iommu_group symlink");
  }

  fs::path target = fs::read_symlink(group_link);
  fs::path group_path =
      target.is_absolute()
          ? target
          : fs::weakly_canonical(group_link.parent_path() / target);

  return {.id = group_path.filename().string(), .path = group_path};
}

static std::vector<fs::path> group_devices(const IommuGroupInfo &group) {
  std::vector<fs::path> devices;
  fs::path devices_dir = group.path / "devices";
  if (!fs::exists(devices_dir))
    return devices;

  for (const fs::directory_entry &entry : fs::directory_iterator(devices_dir)) {
    fs::path target = fs::read_symlink(entry.path());
    fs::path dev_path = target.is_absolute()
                            ? target
                            : fs::weakly_canonical(entry.path().parent_path() /
                                                   target);
    devices.push_back(dev_path);
  }

  std::sort(devices.begin(), devices.end());
  return devices;
}

static void print_group_devices(const IommuGroupInfo &group) {
  std::println("IOMMU group devices:");
  for (const fs::path &dev_path : group_devices(group)) {
    DriverInfo driver = current_driver(dev_path);
    std::println("  {}  driver={}", dev_path.filename().string(), driver.name);
  }
}

static VfioContext open_vfio_context(const std::string &bdf,
                                     const fs::path &dev_dir) {
  IommuGroupInfo group = iommu_group_for_device(dev_dir);
  std::println("IOMMU group: {} ({})", group.id, group.path.string());
  print_group_devices(group);

  VfioContext vfio;
  vfio.container = open_fd("/dev/vfio/vfio", O_RDWR);

  int api_version =
      ioctl_value(vfio.container.get(), VFIO_GET_API_VERSION,
                  "VFIO_GET_API_VERSION");
  if (api_version != VFIO_API_VERSION) {
    throw std::runtime_error("unsupported VFIO API version " +
                             std::to_string(api_version));
  }

  int type1 = ioctl_arg_value(vfio.container.get(), VFIO_CHECK_EXTENSION,
                              VFIO_TYPE1_IOMMU,
                              "VFIO_CHECK_EXTENSION(VFIO_TYPE1_IOMMU)");
  int type1v2 = ioctl_arg_value(vfio.container.get(), VFIO_CHECK_EXTENSION,
                                VFIO_TYPE1v2_IOMMU,
                                "VFIO_CHECK_EXTENSION(VFIO_TYPE1v2_IOMMU)");
  if (!type1 && !type1v2)
    throw std::runtime_error("VFIO Type1 IOMMU is not supported");

  vfio.group = open_fd(fs::path("/dev/vfio") / group.id, O_RDWR);

  vfio_group_status group_status{};
  group_status.argsz = sizeof(group_status);
  ioctl_checked(vfio.group.get(), VFIO_GROUP_GET_STATUS, &group_status,
                "VFIO_GROUP_GET_STATUS");
  if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    throw std::runtime_error(
        "IOMMU group is not viable; every device in the group must be bound "
        "to a VFIO-compatible driver or safely unbound");
  }

  int container_fd = vfio.container.get();
  ioctl_checked(vfio.group.get(), VFIO_GROUP_SET_CONTAINER, &container_fd,
                "VFIO_GROUP_SET_CONTAINER");

  int iommu_type = type1v2 ? VFIO_TYPE1v2_IOMMU : VFIO_TYPE1_IOMMU;
  ioctl_arg_value(vfio.container.get(), VFIO_SET_IOMMU, iommu_type,
                  "VFIO_SET_IOMMU");
  std::println("container IOMMU type: {}",
               iommu_type == VFIO_TYPE1v2_IOMMU ? "VFIO_TYPE1v2_IOMMU"
                                                 : "VFIO_TYPE1_IOMMU");

  int raw_device_fd = ::ioctl(vfio.group.get(), VFIO_GROUP_GET_DEVICE_FD,
                              bdf.c_str());
  if (raw_device_fd == -1) {
    int error = errno;
    throw std::runtime_error("VFIO_GROUP_GET_DEVICE_FD: " +
                             errno_text(error));
  }
  vfio.device = UniqueFd(raw_device_fd);

  return vfio;
}

static ConfigRegion get_config_region(int device_fd) {
  vfio_region_info region{};
  region.argsz = sizeof(region);
  region.index = VFIO_PCI_CONFIG_REGION_INDEX;
  ioctl_checked(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region,
                "VFIO_DEVICE_GET_REGION_INFO(CONFIG)");

  return {.offset = region.offset, .size = region.size, .flags = region.flags};
}

static void pread_exact(int fd, void *data, std::size_t size,
                        std::uint64_t offset, std::string_view what) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    throw std::runtime_error(std::string(what) + ": offset too large");

  auto *out = static_cast<std::uint8_t *>(data);
  std::size_t done = 0;
  while (done < size) {
    ssize_t got = ::pread(fd, out + done, size - done,
                          static_cast<off_t>(offset + done));
    if (got == -1 && errno == EINTR)
      continue;
    if (got == -1) {
      int error = errno;
      throw std::runtime_error(std::string(what) + ": " + errno_text(error));
    }
    if (got == 0)
      throw std::runtime_error(std::string(what) + ": short read");
    done += static_cast<std::size_t>(got);
  }
}

static void read_config_bytes(int device_fd, const ConfigRegion &config,
                              std::uint16_t offset, void *data,
                              std::size_t size) {
  if (static_cast<std::uint64_t>(offset) + size > config.size) {
    throw std::runtime_error("PCI config read past VFIO CONFIG region");
  }
  pread_exact(device_fd, data, size, config.offset + offset, "pread CONFIG");
}

static std::uint8_t read_config_u8(int device_fd, const ConfigRegion &config,
                                   std::uint16_t offset) {
  std::uint8_t value = 0;
  read_config_bytes(device_fd, config, offset, &value, sizeof(value));
  return value;
}

static std::uint16_t read_config_le16(int device_fd, const ConfigRegion &config,
                                      std::uint16_t offset) {
  std::uint8_t bytes[2]{};
  read_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

static std::uint32_t read_config_le32(int device_fd, const ConfigRegion &config,
                                      std::uint16_t offset) {
  std::uint8_t bytes[4]{};
  read_config_bytes(device_fd, config, offset, bytes, sizeof(bytes));
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

static std::uint64_t read_config_le64_from_halves(int device_fd,
                                                  const ConfigRegion &config,
                                                  std::uint16_t lo_offset,
                                                  std::uint16_t hi_offset) {
  std::uint64_t lo = read_config_le32(device_fd, config, lo_offset);
  std::uint64_t hi = read_config_le32(device_fd, config, hi_offset);
  return lo | (hi << 32);
}

static void print_virtio_vendor_cap(int device_fd, const ConfigRegion &config,
                                    std::uint8_t cap_offset,
                                    std::uint8_t cap_len) {
  std::uint8_t cfg_type =
      read_config_u8(device_fd, config, cap_offset + VIRTIO_PCI_CAP_CFG_TYPE);

  std::println("  cap @0x{:02x}: {} ({})", cap_offset,
               virtio_cfg_type_name(cfg_type), cfg_type);
  std::println("    cap_len: {}", cap_len);

  if (cfg_type == VIRTIO_PCI_CAP_VENDOR_CFG) {
    if (cap_len >= 6) {
      std::uint16_t vendor_id = read_config_le16(device_fd, config,
                                                 cap_offset + 4);
      std::println("    vendor_id: 0x{:04x}", vendor_id);
    }
    return;
  }

  if (cap_len < sizeof(virtio_pci_cap)) {
    std::println("    invalid: virtio_pci_cap needs at least {} bytes",
                 sizeof(virtio_pci_cap));
    return;
  }

  std::uint8_t bar =
      read_config_u8(device_fd, config, cap_offset + VIRTIO_PCI_CAP_BAR);
  std::uint8_t cap_id = read_config_u8(device_fd, config, cap_offset + 5);
  std::uint64_t offset =
      read_config_le32(device_fd, config, cap_offset + VIRTIO_PCI_CAP_OFFSET);
  std::uint64_t length =
      read_config_le32(device_fd, config, cap_offset + VIRTIO_PCI_CAP_LENGTH);

  if (cap_len >= sizeof(virtio_pci_cap64)) {
    offset = read_config_le64_from_halves(
        device_fd, config, cap_offset + VIRTIO_PCI_CAP_OFFSET,
        cap_offset + sizeof(virtio_pci_cap));
    length = read_config_le64_from_halves(
        device_fd, config, cap_offset + VIRTIO_PCI_CAP_LENGTH,
        cap_offset + sizeof(virtio_pci_cap) + sizeof(std::uint32_t));
  }

  std::println("    id: {}", cap_id);
  std::println("    bar: {}", bar);
  std::println("    offset: 0x{:x}", offset);
  std::println("    length: 0x{:x} ({})", length, length);

  if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
    if (cap_len >= sizeof(virtio_pci_notify_cap)) {
      std::uint32_t multiplier = read_config_le32(
          device_fd, config, cap_offset + VIRTIO_PCI_NOTIFY_CAP_MULT);
      std::println("    notify_off_multiplier: {}", multiplier);
    } else {
      std::println("    notify_off_multiplier: <missing>");
    }
  }
}

static void show_virtio_caps(const std::string &bdf, const fs::path &dev_dir) {
  require_vfio_driver(dev_dir);

  DriverInfo driver = current_driver(dev_dir);
  std::println("BDF: {}", bdf);
  std::println("driver: {}", driver.name);

  VfioContext vfio = open_vfio_context(bdf, dev_dir);
  ConfigRegion config = get_config_region(vfio.device.get());

  std::println("CONFIG region:");
  std::println("  size: 0x{:x} ({})", config.size, config.size);
  std::println("  offset: 0x{:x}", config.offset);
  std::println("  flags: {}", region_flag_names(config.flags));

  std::uint16_t vendor_id =
      read_config_le16(vfio.device.get(), config, PCI_VENDOR_ID);
  std::uint16_t device_id =
      read_config_le16(vfio.device.get(), config, PCI_DEVICE_ID);
  std::uint16_t status = read_config_le16(vfio.device.get(), config, PCI_STATUS);
  std::uint8_t cap_head =
      read_config_u8(vfio.device.get(), config, PCI_CAPABILITY_LIST);

  std::println("PCI IDs: vendor=0x{:04x} device=0x{:04x}", vendor_id,
               device_id);
  std::println("PCI status: 0x{:04x}", status);

  if (!(status & PCI_STATUS_CAP_LIST)) {
    std::println("PCI capability list: <not present>");
    return;
  }

  std::println("PCI capability list head: 0x{:02x}", cap_head);
  std::println("Virtio PCI capabilities:");

  bool found_virtio = false;
  std::vector<bool> visited(256, false);
  std::uint8_t cap = cap_head;
  for (unsigned hop = 0; cap != 0 && hop < 64; ++hop) {
    cap &= ~0x3u;
    if (cap < 0x40 || static_cast<std::uint64_t>(cap) + 2 > config.size) {
      std::println("  stopped at invalid capability pointer 0x{:02x}", cap);
      break;
    }
    if (visited[cap]) {
      std::println("  stopped at capability loop 0x{:02x}", cap);
      break;
    }
    visited[cap] = true;

    std::uint8_t cap_id = read_config_u8(vfio.device.get(), config,
                                         cap + PCI_CAP_LIST_ID);
    std::uint8_t next = read_config_u8(vfio.device.get(), config,
                                       cap + PCI_CAP_LIST_NEXT);

    if (cap_id == PCI_CAP_ID_VNDR) {
      std::uint8_t cap_len = read_config_u8(vfio.device.get(), config, cap + 2);
      if (static_cast<std::uint64_t>(cap) + cap_len > config.size) {
        std::println("  cap @0x{:02x}: vendor capability exceeds CONFIG region",
                     cap);
      } else if (cap_len >= 4) {
        found_virtio = true;
        print_virtio_vendor_cap(vfio.device.get(), config, cap, cap_len);
      }
    }

    cap = next;
  }

  if (!found_virtio)
    std::println("  <none>");
}

int main(int argc, char *argv[]) {
  if (argc != 3 || std::string_view(argv[2]) != "--show") {
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

    show_virtio_caps(bdf, dev_dir);
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
}
