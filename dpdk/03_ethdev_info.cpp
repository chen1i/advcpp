// Exercise 03: DPDK ethdev capability inspection
//
// Goal: after EAL has probed a PCI device into an ethdev port, inspect the
// read-only capability information that drives later rte_eth_dev_configure()
// choices.
//
// This sample still does not configure RX/TX queues and does not start the
// device. It only reads port metadata and capability bitmaps.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rte_bus.h>
#include <rte_config.h>
#include <rte_dev.h>
#include <rte_devargs.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf_ptype.h>
#include <rte_version.h>

struct Options {
  std::string port_name;
  std::uint16_t port_id = RTE_MAX_ETHPORTS;
  bool have_port_id = false;
  std::uint32_t ptype_mask = RTE_PTYPE_ALL_MASK;
};

struct EalDeviceArg {
  std::string option;
  std::string value;
};

struct NamedFlag64 {
  std::uint64_t bit;
  const char *name;
};

struct NamedFlag32 {
  std::uint32_t bit;
  const char *name;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} [EAL options] -- (--port N | --port-name BDF) [--ptype-mask HEX]\n\n"
      "Examples:\n"
      "  {} -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- --port 0\n"
      "  {} -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- "
      "--port-name 0000:c1:00.6\n\n"
      "Notes:\n"
      "  This sample only reads ethdev capabilities. It does not configure,\n"
      "  start, or stop the port.",
      argv0, argv0, argv0);
}

static int first_app_option_index(const std::vector<std::string> &args) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--")
      return static_cast<int>(i + 1);
  }
  return static_cast<int>(args.size());
}

static std::uint64_t parse_u64(std::string_view text, std::string_view name) {
  std::size_t pos = 0;
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    pos = 2;
    base = 16;
  }

  std::uint64_t value = 0;
  for (; pos < text.size(); ++pos) {
    char c = text[pos];
    unsigned digit = 0;
    if (c >= '0' && c <= '9')
      digit = static_cast<unsigned>(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f')
      digit = static_cast<unsigned>(c - 'a' + 10);
    else if (base == 16 && c >= 'A' && c <= 'F')
      digit = static_cast<unsigned>(c - 'A' + 10);
    else
      throw std::runtime_error("invalid " + std::string(name) + ": " +
                               std::string(text));
    if (digit >= static_cast<unsigned>(base))
      throw std::runtime_error("invalid " + std::string(name) + ": " +
                               std::string(text));
    value = value * static_cast<unsigned>(base) + digit;
  }
  return value;
}

static std::uint16_t parse_port_id(std::string_view text) {
  std::uint64_t value = parse_u64(text, "port");
  if (value >= RTE_MAX_ETHPORTS)
    throw std::runtime_error("port id is outside RTE_MAX_ETHPORTS");
  return static_cast<std::uint16_t>(value);
}

static Options parse_app_options(const std::vector<std::string> &args,
                                 int first_app_arg) {
  Options options;

  for (std::size_t i = static_cast<std::size_t>(first_app_arg);
       i < args.size(); ++i) {
    std::string_view arg = args[i];
    if (arg == "--") {
      continue;
    } else if (arg == "--port") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--port requires a port id");
      options.port_id = parse_port_id(args[++i]);
      options.have_port_id = true;
    } else if (arg.starts_with("--port=")) {
      options.port_id = parse_port_id(arg.substr(7));
      options.have_port_id = true;
    } else if (arg == "--port-name") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--port-name requires a device name");
      options.port_name = args[++i];
    } else if (arg.starts_with("--port-name=")) {
      options.port_name = std::string(arg.substr(12));
    } else if (arg == "--ptype-mask") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--ptype-mask requires a value");
      options.ptype_mask = static_cast<std::uint32_t>(
          parse_u64(args[++i], "ptype-mask"));
    } else if (arg.starts_with("--ptype-mask=")) {
      options.ptype_mask =
          static_cast<std::uint32_t>(parse_u64(arg.substr(13), "ptype-mask"));
    } else if (arg == "-h" || arg == "--help") {
      usage(args[0].c_str());
      std::exit(0);
    } else {
      throw std::runtime_error("unknown application option: " +
                               std::string(arg));
    }
  }

  if (options.have_port_id && !options.port_name.empty())
    throw std::runtime_error("use either --port or --port-name, not both");
  if (!options.have_port_id && options.port_name.empty())
    throw std::runtime_error("provide --port or --port-name after --");
  return options;
}

static bool has_no_pci_option(const std::vector<std::string> &args,
                              int first_app_arg) {
  for (int i = 1; i < first_app_arg; ++i) {
    std::string_view arg = args[static_cast<std::size_t>(i)];
    if (arg == "--no-pci")
      return true;
  }
  return false;
}

static std::vector<EalDeviceArg>
collect_eal_device_args(const std::vector<std::string> &args,
                        int first_app_arg) {
  std::vector<EalDeviceArg> found;

  for (int i = 1; i < first_app_arg; ++i) {
    std::string_view arg = args[static_cast<std::size_t>(i)];
    if (arg == "-a" || arg == "--allow" || arg == "-b" || arg == "--block") {
      if (i + 1 < first_app_arg)
        found.push_back({std::string(arg), args[static_cast<std::size_t>(++i)]});
      continue;
    }
    if (arg.starts_with("--allow="))
      found.push_back({"--allow", std::string(arg.substr(8))});
    else if (arg.starts_with("--block="))
      found.push_back({"--block", std::string(arg.substr(8))});
    else if (arg.starts_with("-a") && arg.size() > 2)
      found.push_back({"-a", std::string(arg.substr(2))});
    else if (arg.starts_with("-b") && arg.size() > 2)
      found.push_back({"-b", std::string(arg.substr(2))});
  }
  return found;
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

static const char *safe_cstr(const char *text) { return text ? text : ""; }

static std::uint64_t bit(unsigned n) { return UINT64_C(1) << n; }

static void print_named_flags64(std::string_view label, std::uint64_t value,
                                const std::vector<NamedFlag64> &flags) {
  std::println("{}: 0x{:016x}", label, value);
  if (value == 0) {
    std::println("  <none>");
    return;
  }

  std::uint64_t known = 0;
  for (const NamedFlag64 &flag : flags) {
    known |= flag.bit;
    if ((value & flag.bit) != 0)
      std::println("  {}", flag.name);
  }

  std::uint64_t unknown = value & ~known;
  if (unknown != 0)
    std::println("  unknown bits: 0x{:016x}", unknown);
}

static void print_named_flags32(std::string_view label, std::uint32_t value,
                                const std::vector<NamedFlag32> &flags) {
  std::println("{}: 0x{:08x}", label, value);
  if (value == 0) {
    std::println("  <none>");
    return;
  }

  std::uint32_t known = 0;
  for (const NamedFlag32 &flag : flags) {
    known |= flag.bit;
    if ((value & flag.bit) != 0)
      std::println("  {}", flag.name);
  }

  std::uint32_t unknown = value & ~known;
  if (unknown != 0)
    std::println("  unknown bits: 0x{:08x}", unknown);
}

static void print_offloads(std::string_view label, std::uint64_t value,
                           bool rx) {
  std::println("{}: 0x{:016x}", label, value);
  if (value == 0) {
    std::println("  <none>");
    return;
  }

  for (unsigned bit_index = 0; bit_index < 64; ++bit_index) {
    std::uint64_t flag = bit(bit_index);
    if ((value & flag) == 0)
      continue;
    const char *name = rx ? rte_eth_dev_rx_offload_name(flag)
                          : rte_eth_dev_tx_offload_name(flag);
    std::println("  bit {:2}: {}", bit_index, safe_cstr(name));
  }
}

static void print_dev_capabilities(std::uint64_t value) {
  print_named_flags64(
      "dev_capa", value,
      {
          {RTE_ETH_DEV_CAPA_RUNTIME_RX_QUEUE_SETUP,
           "RUNTIME_RX_QUEUE_SETUP"},
          {RTE_ETH_DEV_CAPA_RUNTIME_TX_QUEUE_SETUP,
           "RUNTIME_TX_QUEUE_SETUP"},
          {RTE_ETH_DEV_CAPA_RXQ_SHARE, "RXQ_SHARE"},
          {RTE_ETH_DEV_CAPA_FLOW_RULE_KEEP, "FLOW_RULE_KEEP"},
          {RTE_ETH_DEV_CAPA_FLOW_SHARED_OBJECT_KEEP,
           "FLOW_SHARED_OBJECT_KEEP"},
      });
}

static void print_desc_limits(std::string_view label,
                              const rte_eth_desc_lim &lim) {
  std::println("{} descriptor limits:", label);
  std::println("  nb_min: {}", lim.nb_min);
  std::println("  nb_max: {}", lim.nb_max);
  std::println("  nb_align: {}", lim.nb_align);
  std::println("  nb_seg_max: {}", lim.nb_seg_max);
  std::println("  nb_mtu_seg_max: {}", lim.nb_mtu_seg_max);
}

static std::vector<NamedFlag32> speed_flags() {
  return {
      {RTE_ETH_LINK_SPEED_FIXED, "FIXED"},
      {RTE_ETH_LINK_SPEED_10M_HD, "10M_HD"},
      {RTE_ETH_LINK_SPEED_10M, "10M"},
      {RTE_ETH_LINK_SPEED_100M_HD, "100M_HD"},
      {RTE_ETH_LINK_SPEED_100M, "100M"},
      {RTE_ETH_LINK_SPEED_1G, "1G"},
      {RTE_ETH_LINK_SPEED_2_5G, "2.5G"},
      {RTE_ETH_LINK_SPEED_5G, "5G"},
      {RTE_ETH_LINK_SPEED_10G, "10G"},
      {RTE_ETH_LINK_SPEED_20G, "20G"},
      {RTE_ETH_LINK_SPEED_25G, "25G"},
      {RTE_ETH_LINK_SPEED_40G, "40G"},
      {RTE_ETH_LINK_SPEED_50G, "50G"},
      {RTE_ETH_LINK_SPEED_56G, "56G"},
      {RTE_ETH_LINK_SPEED_100G, "100G"},
      {RTE_ETH_LINK_SPEED_200G, "200G"},
      {RTE_ETH_LINK_SPEED_400G, "400G"},
      {RTE_ETH_LINK_SPEED_800G, "800G"},
  };
}

static std::vector<NamedFlag64> rss_flags() {
  return {
      {RTE_ETH_RSS_IPV4, "IPV4"},
      {RTE_ETH_RSS_FRAG_IPV4, "FRAG_IPV4"},
      {RTE_ETH_RSS_NONFRAG_IPV4_TCP, "NONFRAG_IPV4_TCP"},
      {RTE_ETH_RSS_NONFRAG_IPV4_UDP, "NONFRAG_IPV4_UDP"},
      {RTE_ETH_RSS_NONFRAG_IPV4_SCTP, "NONFRAG_IPV4_SCTP"},
      {RTE_ETH_RSS_NONFRAG_IPV4_OTHER, "NONFRAG_IPV4_OTHER"},
      {RTE_ETH_RSS_IPV6, "IPV6"},
      {RTE_ETH_RSS_FRAG_IPV6, "FRAG_IPV6"},
      {RTE_ETH_RSS_NONFRAG_IPV6_TCP, "NONFRAG_IPV6_TCP"},
      {RTE_ETH_RSS_NONFRAG_IPV6_UDP, "NONFRAG_IPV6_UDP"},
      {RTE_ETH_RSS_NONFRAG_IPV6_SCTP, "NONFRAG_IPV6_SCTP"},
      {RTE_ETH_RSS_NONFRAG_IPV6_OTHER, "NONFRAG_IPV6_OTHER"},
      {RTE_ETH_RSS_L2_PAYLOAD, "L2_PAYLOAD"},
      {RTE_ETH_RSS_IPV6_EX, "IPV6_EX"},
      {RTE_ETH_RSS_IPV6_TCP_EX, "IPV6_TCP_EX"},
      {RTE_ETH_RSS_IPV6_UDP_EX, "IPV6_UDP_EX"},
      {RTE_ETH_RSS_PORT, "PORT"},
      {RTE_ETH_RSS_VXLAN, "VXLAN"},
      {RTE_ETH_RSS_GENEVE, "GENEVE"},
      {RTE_ETH_RSS_NVGRE, "NVGRE"},
      {RTE_ETH_RSS_GTPU, "GTPU"},
      {RTE_ETH_RSS_ETH, "ETH"},
      {RTE_ETH_RSS_S_VLAN, "S_VLAN"},
      {RTE_ETH_RSS_C_VLAN, "C_VLAN"},
      {RTE_ETH_RSS_ESP, "ESP"},
      {RTE_ETH_RSS_AH, "AH"},
      {RTE_ETH_RSS_L2TPV3, "L2TPV3"},
      {RTE_ETH_RSS_PFCP, "PFCP"},
      {RTE_ETH_RSS_PPPOE, "PPPOE"},
      {RTE_ETH_RSS_ECPRI, "ECPRI"},
      {RTE_ETH_RSS_MPLS, "MPLS"},
      {RTE_ETH_RSS_IPV4_CHKSUM, "IPV4_CHKSUM"},
      {RTE_ETH_RSS_L4_CHKSUM, "L4_CHKSUM"},
      {RTE_ETH_RSS_L2TPV2, "L2TPV2"},
      {RTE_ETH_RSS_IPV6_FLOW_LABEL, "IPV6_FLOW_LABEL"},
      {RTE_ETH_RSS_IB_BTH, "IB_BTH"},
  };
}

static void print_rss_algorithms(std::uint32_t value) {
  print_named_flags32(
      "rss_algo_capa", value,
      {
          {RTE_ETH_HASH_ALGO_CAPA_MASK(DEFAULT), "DEFAULT"},
          {RTE_ETH_HASH_ALGO_CAPA_MASK(TOEPLITZ), "TOEPLITZ"},
          {RTE_ETH_HASH_ALGO_CAPA_MASK(SIMPLE_XOR), "SIMPLE_XOR"},
          {RTE_ETH_HASH_ALGO_CAPA_MASK(SYMMETRIC_TOEPLITZ),
           "SYMMETRIC_TOEPLITZ"},
          {RTE_ETH_HASH_ALGO_CAPA_MASK(SYMMETRIC_TOEPLITZ_SORT),
           "SYMMETRIC_TOEPLITZ_SORT"},
      });
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

static void print_supported_ptypes(std::uint16_t port_id,
                                   std::uint32_t ptype_mask) {
  int count = rte_eth_dev_get_supported_ptypes(port_id, ptype_mask, nullptr, 0);
  std::println("Supported packet types:");
  std::println("  requested mask: 0x{:08x}", ptype_mask);
  if (count < 0) {
    std::println("  rte_eth_dev_get_supported_ptypes failed: {}",
                 rte_strerror(-count));
    return;
  }
  if (count == 0) {
    std::println("  <none reported>");
    return;
  }

  std::vector<std::uint32_t> ptypes(static_cast<std::size_t>(count) + 1);
  int filled =
      rte_eth_dev_get_supported_ptypes(port_id, ptype_mask, ptypes.data(),
                                       static_cast<int>(ptypes.size()));
  if (filled < 0) {
    std::println("  second query failed: {}", rte_strerror(-filled));
    return;
  }

  for (int i = 0; i < filled; ++i) {
    std::array<char, 128> ptype_name{};
    int ret = rte_get_ptype_name(ptypes[static_cast<std::size_t>(i)],
                                 ptype_name.data(), ptype_name.size());
    if (ret < 0)
      std::snprintf(ptype_name.data(), ptype_name.size(), "UNKNOWN");
    std::println("  0x{:08x}: {}", ptypes[static_cast<std::size_t>(i)],
                 ptype_name.data());
  }
}

static std::uint16_t resolve_port(const Options &options) {
  if (options.have_port_id) {
    if (!rte_eth_dev_is_valid_port(options.port_id))
      throw std::runtime_error("requested port id is not valid");
    return options.port_id;
  }

  std::uint16_t port_id = RTE_MAX_ETHPORTS;
  int ret = rte_eth_dev_get_port_by_name(options.port_name.c_str(), &port_id);
  if (ret != 0) {
    throw std::runtime_error("cannot resolve port-name " + options.port_name +
                             ": " + rte_strerror(-ret));
  }
  return port_id;
}

static void print_eal_summary(const std::vector<EalDeviceArg> &device_args,
                              bool no_pci) {
  std::println("EAL PCI selection:");
  std::println("  --no-pci: {}", no_pci ? "yes" : "no");
  if (device_args.empty()) {
    std::println("  allow/block options: <none>");
    return;
  }
  std::println("  allow/block options:");
  for (const EalDeviceArg &arg : device_args)
    std::println("    {} {}", arg.option, arg.value);
}

static void print_basic_port_identity(std::uint16_t port_id,
                                      const rte_eth_dev_info &info) {
  std::array<char, RTE_ETH_NAME_MAX_LEN> port_name{};
  int ret = rte_eth_dev_get_name_by_port(port_id, port_name.data());
  if (ret != 0)
    std::snprintf(port_name.data(), port_name.size(), "<unknown>");

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

  std::println("Port identity:");
  std::println("  port id: {}", port_id);
  std::println("  name: {}", port_name.data());
  std::println("  driver: {}", safe_cstr(info.driver_name));
  std::println("  if_index: {}", info.if_index);
  std::println("  socket: {}", rte_eth_dev_socket_id(port_id));
  std::println("  mac: {}", mac_text.data());
  if (have_link) {
    std::println("  link: {} speed={}Mbps duplex={}",
                 link_status_name(link), link.link_speed, duplex_name(link));
  } else {
    std::println("  link: rte_eth_link_get_nowait failed: {}",
                 rte_strerror(-ret));
  }

  const rte_device *device = info.device;
  if (device) {
    const rte_bus *bus = rte_dev_bus(device);
    const rte_driver *driver = rte_dev_driver(device);
    std::println("  rte_device.name: {}", safe_cstr(rte_dev_name(device)));
    std::println("  rte_device.bus: {}",
                 bus ? safe_cstr(rte_bus_name(bus)) : "");
    std::println("  rte_device.bus_info: {}", safe_cstr(rte_dev_bus_info(device)));
    std::println("  rte_device.driver: {}",
                 driver ? safe_cstr(rte_driver_name(driver)) : "");
    std::println("  rte_device.numa_node: {}", rte_dev_numa_node(device));
  }
}

static void print_queue_and_mtu_info(const rte_eth_dev_info &info) {
  std::println("Queue and packet-size limits:");
  std::println("  min_mtu: {}", info.min_mtu);
  std::println("  max_mtu: {}", info.max_mtu);
  std::println("  min_rx_bufsize: {}", info.min_rx_bufsize);
  std::println("  max_rx_bufsize: {}", info.max_rx_bufsize);
  std::println("  max_rx_pktlen: {}", info.max_rx_pktlen);
  std::println("  max_lro_pkt_size: {}", info.max_lro_pkt_size);
  std::println("  max_rx_queues: {}", info.max_rx_queues);
  std::println("  max_tx_queues: {}", info.max_tx_queues);
  std::println("  configured nb_rx_queues: {}", info.nb_rx_queues);
  std::println("  configured nb_tx_queues: {}", info.nb_tx_queues);
  std::println("  max_rx_mempools: {}", info.max_rx_mempools);
  std::println("  max_mac_addrs: {}", info.max_mac_addrs);
  std::println("  max_hash_mac_addrs: {}", info.max_hash_mac_addrs);
  std::println("  max_vfs: {}", info.max_vfs);
  std::println("  max_vmdq_pools: {}", info.max_vmdq_pools);
}

static void print_defaults(const rte_eth_dev_info &info) {
  std::println("Default queue recommendations:");
  std::println("  default_rxportconf.burst_size: {}",
               info.default_rxportconf.burst_size);
  std::println("  default_rxportconf.ring_size: {}",
               info.default_rxportconf.ring_size);
  std::println("  default_rxportconf.nb_queues: {}",
               info.default_rxportconf.nb_queues);
  std::println("  default_txportconf.burst_size: {}",
               info.default_txportconf.burst_size);
  std::println("  default_txportconf.ring_size: {}",
               info.default_txportconf.ring_size);
  std::println("  default_txportconf.nb_queues: {}",
               info.default_txportconf.nb_queues);
}

static void print_ethdev_info(std::uint16_t port_id,
                              std::uint32_t ptype_mask) {
  rte_eth_dev_info info{};
  int ret = rte_eth_dev_info_get(port_id, &info);
  if (ret != 0)
    throw std::runtime_error("rte_eth_dev_info_get failed: " +
                             std::string(rte_strerror(-ret)));

  print_basic_port_identity(port_id, info);
  print_queue_and_mtu_info(info);
  print_desc_limits("RX", info.rx_desc_lim);
  print_desc_limits("TX", info.tx_desc_lim);
  print_defaults(info);
  print_offloads("rx_offload_capa", info.rx_offload_capa, true);
  print_offloads("rx_queue_offload_capa", info.rx_queue_offload_capa, true);
  print_offloads("tx_offload_capa", info.tx_offload_capa, false);
  print_offloads("tx_queue_offload_capa", info.tx_queue_offload_capa, false);
  print_dev_capabilities(info.dev_capa);
  print_named_flags32("speed_capa", info.speed_capa, speed_flags());
  std::println("RSS capability summary:");
  std::println("  reta_size: {}", info.reta_size);
  std::println("  hash_key_size: {}", info.hash_key_size);
  print_rss_algorithms(info.rss_algo_capa);
  print_named_flags64("flow_type_rss_offloads", info.flow_type_rss_offloads,
                      rss_flags());
  print_supported_ptypes(port_id, ptype_mask);
}

int main(int argc, char **argv) {
  std::vector<std::string> original_args(argv, argv + argc);

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    }
  }

  const int first_app_arg = first_app_option_index(original_args);
  const std::vector<EalDeviceArg> device_args =
      collect_eal_device_args(original_args, first_app_arg);
  const bool no_pci = has_no_pci_option(original_args, first_app_arg);

  int eal_argc = rte_eal_init(argc, argv);
  if (eal_argc < 0) {
    std::println(std::cerr, "rte_eal_init failed: {}",
                 rte_strerror(rte_errno));
    return 1;
  }

  try {
    Options options = parse_app_options(original_args, first_app_arg);
    std::uint16_t port_id = resolve_port(options);

    std::println("DPDK ethdev capability inspection");
    std::println("  version: {}", rte_version());
    std::println("  EAL consumed argv entries: {}", eal_argc);
    std::println("  IOVA mode: {}", iova_mode_name(rte_eal_iova_mode()));
    print_eal_summary(device_args, no_pci);
    print_ethdev_info(port_id, options.ptype_mask);

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
