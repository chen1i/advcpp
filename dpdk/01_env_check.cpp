// Exercise 01: DPDK environment check
//
// Goal: initialize DPDK EAL and print the runtime facts that usually decide
// whether later packet I/O samples will work: hugepages, lcores, IOVA mode,
// VFIO visibility, and probed Ethernet devices.
//
// This sample intentionally does not configure RX/TX queues and does not send
// packets.  It is a safe first smoke test for a DPDK installation.

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_memzone.h>
#include <rte_version.h>

namespace fs = std::filesystem;

struct Options {
  bool dump_memzones = false;
};

struct Meminfo {
  std::optional<std::uint64_t> hugepages_total;
  std::optional<std::uint64_t> hugepages_free;
  std::optional<std::uint64_t> hugepages_reserved;
  std::optional<std::uint64_t> hugepages_surplus;
  std::optional<std::uint64_t> hugepage_size_kb;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} [EAL options] [--] [--dump-memzones]\n\n"
      "Examples:\n"
      "  {} -l 0-1 -n 4\n"
      "  {} -l 0-1 -n 4 --log-level=lib.eal:info -- --dump-memzones\n\n"
      "Notes:\n"
      "  EAL options are parsed by rte_eal_init(). Application options must\n"
      "  appear after -- if they could be confused with EAL options.",
      argv0, argv0, argv0);
}

static std::optional<std::uint64_t> parse_first_u64(std::string_view line) {
  std::size_t first_digit = line.find_first_of("0123456789");
  if (first_digit == std::string_view::npos)
    return std::nullopt;

  std::uint64_t value = 0;
  const char *begin = line.data() + first_digit;
  const char *end = line.data() + line.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{})
    return std::nullopt;
  return value;
}

static Meminfo read_meminfo() {
  std::ifstream input("/proc/meminfo");
  if (!input)
    throw std::system_error(errno, std::generic_category(),
                            "open /proc/meminfo");

  Meminfo info;
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with("HugePages_Total:"))
      info.hugepages_total = parse_first_u64(line);
    else if (line.starts_with("HugePages_Free:"))
      info.hugepages_free = parse_first_u64(line);
    else if (line.starts_with("HugePages_Rsvd:"))
      info.hugepages_reserved = parse_first_u64(line);
    else if (line.starts_with("HugePages_Surp:"))
      info.hugepages_surplus = parse_first_u64(line);
    else if (line.starts_with("Hugepagesize:"))
      info.hugepage_size_kb = parse_first_u64(line);
  }
  return info;
}

static std::string optional_u64(std::optional<std::uint64_t> value) {
  return value ? std::to_string(*value) : std::string("<unknown>");
}

static std::string access_status(const fs::path &path, int mode) {
  if (!fs::exists(path))
    return "missing";
  if (::access(path.c_str(), mode) == 0)
    return "ok";
  return std::string("not accessible: ") + std::strerror(errno);
}

static std::string iova_mode_name(rte_iova_mode mode) {
  switch (mode) {
  case RTE_IOVA_PA:
    return "PA";
  case RTE_IOVA_VA:
    return "VA";
  case RTE_IOVA_DC:
    return "don't care";
  default:
    return "unknown";
  }
}

static std::string link_status_name(const rte_eth_link &link) {
  if (link.link_status == RTE_ETH_LINK_DOWN)
    return "down";
  return "up";
}

static std::string duplex_name(const rte_eth_link &link) {
  if (link.link_status == RTE_ETH_LINK_DOWN)
    return "n/a";
  return link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half";
}

static Options parse_app_options(const std::vector<std::string> &args,
                                 int first_app_arg) {
  Options options;

  for (std::size_t i = static_cast<std::size_t>(first_app_arg);
       i < args.size(); ++i) {
    std::string_view arg = args[i];
    if (arg == "--") {
      continue;
    } else if (arg == "--dump-memzones") {
      options.dump_memzones = true;
    } else if (arg == "-h" || arg == "--help") {
      usage(args[0].c_str());
      std::exit(0);
    } else {
      throw std::runtime_error("unknown application option: " +
                               std::string(arg));
    }
  }

  return options;
}

static int first_app_option_index(const std::vector<std::string> &args) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--")
      return static_cast<int>(i + 1);
  }
  return static_cast<int>(args.size());
}

static void print_hugepage_summary() {
  Meminfo meminfo = read_meminfo();

  std::println("Hugepage summary from /proc/meminfo:");
  std::println("  HugePages_Total: {}", optional_u64(meminfo.hugepages_total));
  std::println("  HugePages_Free: {}", optional_u64(meminfo.hugepages_free));
  std::println("  HugePages_Rsvd: {}",
               optional_u64(meminfo.hugepages_reserved));
  std::println("  HugePages_Surp: {}",
               optional_u64(meminfo.hugepages_surplus));
  std::println("  Hugepagesize: {} kB",
               optional_u64(meminfo.hugepage_size_kb));

  if (meminfo.hugepages_total && *meminfo.hugepages_total == 0) {
    std::println("  note: no preallocated hugepages are visible");
  }
}

static void print_vfio_summary() {
  std::println("VFIO summary:");
  std::println("  /dev/vfio/vfio: {}",
               access_status("/dev/vfio/vfio", R_OK | W_OK));
  std::println("  /sys/kernel/iommu_groups: {}",
               fs::exists("/sys/kernel/iommu_groups") ? "present" : "missing");
  std::println("  vfio-pci module: {}",
               fs::exists("/sys/module/vfio_pci") ? "loaded" : "not loaded");
}

static void print_lcore_summary() {
  std::println("Lcore summary:");
  std::println("  rte_lcore_count: {}", rte_lcore_count());
  std::println("  main lcore: {}", rte_get_main_lcore());
  std::println("  current lcore: {}", rte_lcore_id());
  std::println("  process socket id: {}", rte_socket_id());

  unsigned int lcore_id = 0;
  RTE_LCORE_FOREACH(lcore_id) {
    std::println("  lcore {}: socket {}", lcore_id,
                 rte_lcore_to_socket_id(lcore_id));
  }
}

static void print_port_summary() {
  std::uint16_t port_count = rte_eth_dev_count_avail();
  std::println("Ethernet device summary:");
  std::println("  available ports: {}", port_count);

  std::uint16_t port_id = 0;
  RTE_ETH_FOREACH_DEV(port_id) {
    rte_eth_dev_info info{};
    int ret = rte_eth_dev_info_get(port_id, &info);
    if (ret != 0) {
      std::println("  port {}: rte_eth_dev_info_get failed: {}", port_id,
                   rte_strerror(-ret));
      continue;
    }

    rte_ether_addr mac{};
    ret = rte_eth_macaddr_get(port_id, &mac);
    std::array<char, RTE_ETHER_ADDR_FMT_SIZE> mac_text{};
    if (ret == 0)
      rte_ether_format_addr(mac_text.data(), mac_text.size(), &mac);
    else
      std::snprintf(mac_text.data(), mac_text.size(), "<unknown>");

    rte_eth_link link{};
    ret = rte_eth_link_get_nowait(port_id, &link);
    bool have_link = ret == 0;

    std::println("  port {}:", port_id);
    std::println("    driver: {}", info.driver_name ? info.driver_name : "");
    std::println("    if_index: {}", info.if_index);
    std::println("    socket: {}", rte_eth_dev_socket_id(port_id));
    std::println("    mac: {}", mac_text.data());
    std::println("    max_rx_queues: {}", info.max_rx_queues);
    std::println("    max_tx_queues: {}", info.max_tx_queues);
    std::println("    min_rx_bufsize: {}", info.min_rx_bufsize);
    std::println("    max_rx_pktlen: {}", info.max_rx_pktlen);
    if (have_link) {
      std::println("    link: {} speed={}Mbps duplex={}",
                   link_status_name(link), link.link_speed,
                   duplex_name(link));
    } else {
      std::println("    link: rte_eth_link_get_nowait failed: {}",
                   rte_strerror(-ret));
    }
  }
}

int main(int argc, char **argv) {
  std::vector<std::string> original_args(argv, argv + argc);

  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        usage(argv[0]);
        return 0;
      }
    }
  }

  int eal_argc = rte_eal_init(argc, argv);
  if (eal_argc < 0) {
    std::println(std::cerr, "rte_eal_init failed: {}",
                 rte_strerror(rte_errno));
    return 1;
  }

  try {
    Options options = parse_app_options(
        original_args, first_app_option_index(original_args));

    std::println("DPDK environment check");
    std::println("  version: {}", rte_version());
    std::println("  EAL consumed argv entries: {}", eal_argc);
    std::println("  IOVA mode: {}", iova_mode_name(rte_eal_iova_mode()));

    print_hugepage_summary();
    print_vfio_summary();
    print_lcore_summary();
    print_port_summary();

    if (options.dump_memzones) {
      std::println("Memzone dump:");
      rte_memzone_dump(stdout);
    }

    int cleanup = rte_eal_cleanup();
    if (cleanup != 0) {
      std::println(std::cerr, "rte_eal_cleanup failed: {}",
                   rte_strerror(rte_errno));
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    rte_eal_cleanup();
    return 1;
  }
}
