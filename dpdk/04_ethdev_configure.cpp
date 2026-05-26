// Exercise 04: DPDK ethdev configure
//
// Goal: move from read-only port inspection to the first ethdev state change:
// rte_eth_dev_configure().
//
// This sample configures the number of RX/TX queues and a minimal
// rte_eth_conf, then stops. It does not set up queue rings, create mempools,
// start the device, or send packets. Those are separate steps in later samples.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rte_bus.h>
#include <rte_config.h>
#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_version.h>

struct Options {
  std::string port_name;
  std::uint16_t port_id = RTE_MAX_ETHPORTS;
  bool have_port_id = false;
  std::uint16_t rx_queues = 1;
  std::uint16_t tx_queues = 1;
  std::optional<std::uint32_t> mtu;
  std::uint64_t rx_offloads = 0;
  std::uint64_t tx_offloads = 0;
  bool yes = false;
};

struct EalDeviceArg {
  std::string option;
  std::string value;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} [EAL options] -- (--port N | --port-name BDF) [options] [--yes]\n\n"
      "Application options:\n"
      "  --rx-queues N       RX queues to pass to rte_eth_dev_configure "
      "(default: 1)\n"
      "  --tx-queues N       TX queues to pass to rte_eth_dev_configure "
      "(default: 1)\n"
      "  --mtu N             Requested rxmode.mtu; omit to keep PMD default\n"
      "  --rx-offloads HEX   Port RX offloads bitmap; must fit capability\n"
      "  --tx-offloads HEX   Port TX offloads bitmap; must fit capability\n"
      "  --yes               Actually call rte_eth_dev_configure()\n\n"
      "Examples:\n"
      "  {} -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- "
      "--port-name 0000:c1:00.6\n"
      "  {} -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- "
      "--port-name 0000:c1:00.6 --yes\n\n"
      "Notes:\n"
      "  Without --yes this is a dry run. With --yes the sample configures the\n"
      "  port and then closes it before EAL cleanup. It still does not set up\n"
      "  RX/TX queues and does not start packet I/O.",
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
  if (text.empty())
    throw std::runtime_error("empty " + std::string(name));

  std::size_t pos = 0;
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    pos = 2;
    base = 16;
    if (pos == text.size())
      throw std::runtime_error("invalid " + std::string(name) + ": " +
                               std::string(text));
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
    if (value >
        (std::numeric_limits<std::uint64_t>::max() - digit) /
            static_cast<unsigned>(base))
      throw std::runtime_error(std::string(name) + " is too large");
    value = value * static_cast<unsigned>(base) + digit;
  }
  return value;
}

static std::uint16_t parse_u16(std::string_view text, std::string_view name) {
  std::uint64_t value = parse_u64(text, name);
  if (value > std::numeric_limits<std::uint16_t>::max())
    throw std::runtime_error(std::string(name) + " is too large");
  return static_cast<std::uint16_t>(value);
}

static std::uint32_t parse_u32(std::string_view text, std::string_view name) {
  std::uint64_t value = parse_u64(text, name);
  if (value > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error(std::string(name) + " is too large");
  return static_cast<std::uint32_t>(value);
}

static std::uint16_t parse_port_id(std::string_view text) {
  std::uint16_t port = parse_u16(text, "port");
  if (port >= RTE_MAX_ETHPORTS)
    throw std::runtime_error("port id is outside RTE_MAX_ETHPORTS");
  return port;
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
    } else if (arg == "--rx-queues") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--rx-queues requires a count");
      options.rx_queues = parse_u16(args[++i], "rx-queues");
    } else if (arg.starts_with("--rx-queues=")) {
      options.rx_queues = parse_u16(arg.substr(12), "rx-queues");
    } else if (arg == "--tx-queues") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--tx-queues requires a count");
      options.tx_queues = parse_u16(args[++i], "tx-queues");
    } else if (arg.starts_with("--tx-queues=")) {
      options.tx_queues = parse_u16(arg.substr(12), "tx-queues");
    } else if (arg == "--mtu") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--mtu requires a value");
      options.mtu = parse_u32(args[++i], "mtu");
    } else if (arg.starts_with("--mtu=")) {
      options.mtu = parse_u32(arg.substr(6), "mtu");
    } else if (arg == "--rx-offloads") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--rx-offloads requires a hex bitmap");
      options.rx_offloads = parse_u64(args[++i], "rx-offloads");
    } else if (arg.starts_with("--rx-offloads=")) {
      options.rx_offloads = parse_u64(arg.substr(14), "rx-offloads");
    } else if (arg == "--tx-offloads") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--tx-offloads requires a hex bitmap");
      options.tx_offloads = parse_u64(args[++i], "tx-offloads");
    } else if (arg.starts_with("--tx-offloads=")) {
      options.tx_offloads = parse_u64(arg.substr(14), "tx-offloads");
    } else if (arg == "--yes") {
      options.yes = true;
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
  if (options.rx_queues == 0 || options.tx_queues == 0)
    throw std::runtime_error("rx-queues and tx-queues must both be non-zero");
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

static std::string dpdk_error(int ret) {
  if (ret < 0)
    return rte_strerror(-ret);
  if (ret == 0)
    return "success";
  return rte_strerror(ret);
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
                             ": " + dpdk_error(ret));
  }
  return port_id;
}

static void print_eal_summary(const std::vector<EalDeviceArg> &device_args,
                              bool no_pci) {
  std::println("EAL PCI selection:");
  std::println("  --no-pci: {}", no_pci ? "yes" : "no");
  if (device_args.empty()) {
    std::println("  allow/block options: <none>");
    if (!no_pci) {
      std::println("  note: without -a/--allow, EAL may probe every eligible "
                   "PCI device");
    }
    return;
  }
  std::println("  allow/block options:");
  for (const EalDeviceArg &arg : device_args)
    std::println("    {} {}", arg.option, arg.value);
}

static void print_port_identity(std::uint16_t port_id,
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

  std::println("Port identity:");
  std::println("  port id: {}", port_id);
  std::println("  name: {}", port_name.data());
  std::println("  driver: {}", safe_cstr(info.driver_name));
  std::println("  socket: {}", rte_eth_dev_socket_id(port_id));
  std::println("  mac: {}", mac_text.data());
  if (ret == 0) {
    std::println("  link: {} speed={}Mbps duplex={}",
                 link_status_name(link), link.link_speed, duplex_name(link));
  } else {
    std::println("  link: rte_eth_link_get_nowait failed: {}",
                 dpdk_error(ret));
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

static void print_queue_state(std::string_view label,
                              const rte_eth_dev_info &info) {
  std::println("{}:", label);
  std::println("  max_rx_queues: {}", info.max_rx_queues);
  std::println("  max_tx_queues: {}", info.max_tx_queues);
  std::println("  configured nb_rx_queues: {}", info.nb_rx_queues);
  std::println("  configured nb_tx_queues: {}", info.nb_tx_queues);
  std::println("  min_mtu: {}", info.min_mtu);
  std::println("  max_mtu: {}", info.max_mtu);
  std::println("  rx_offload_capa: 0x{:016x}", info.rx_offload_capa);
  std::println("  tx_offload_capa: 0x{:016x}", info.tx_offload_capa);
  std::println("  RX desc limits: min={} max={} align={}",
               info.rx_desc_lim.nb_min, info.rx_desc_lim.nb_max,
               info.rx_desc_lim.nb_align);
  std::println("  TX desc limits: min={} max={} align={}",
               info.tx_desc_lim.nb_min, info.tx_desc_lim.nb_max,
               info.tx_desc_lim.nb_align);
}

static rte_eth_dev_info get_dev_info(std::uint16_t port_id) {
  rte_eth_dev_info info{};
  int ret = rte_eth_dev_info_get(port_id, &info);
  if (ret != 0)
    throw std::runtime_error("rte_eth_dev_info_get failed: " +
                             dpdk_error(ret));
  return info;
}

static void validate_requested_config(const Options &options,
                                      const rte_eth_dev_info &info) {
  if (options.rx_queues > info.max_rx_queues) {
    throw std::runtime_error("requested rx-queues exceeds max_rx_queues");
  }
  if (options.tx_queues > info.max_tx_queues) {
    throw std::runtime_error("requested tx-queues exceeds max_tx_queues");
  }
  if ((options.rx_offloads & ~info.rx_offload_capa) != 0) {
    throw std::runtime_error("requested rx-offloads include unsupported bits");
  }
  if ((options.tx_offloads & ~info.tx_offload_capa) != 0) {
    throw std::runtime_error("requested tx-offloads include unsupported bits");
  }
  if (options.mtu) {
    if (*options.mtu < info.min_mtu || *options.mtu > info.max_mtu)
      throw std::runtime_error("requested mtu is outside min_mtu..max_mtu");
  }
}

static rte_eth_conf make_eth_conf(const Options &options) {
  rte_eth_conf conf{};
  conf.link_speeds = RTE_ETH_LINK_SPEED_AUTONEG;
  conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  conf.rxmode.offloads = options.rx_offloads;
  if (options.mtu)
    conf.rxmode.mtu = *options.mtu;
  conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  conf.txmode.offloads = options.tx_offloads;
  return conf;
}

static void print_config_plan(const Options &options,
                              const rte_eth_conf &conf) {
  std::println("Requested rte_eth_dev_configure plan:");
  std::println("  rx queues: {}", options.rx_queues);
  std::println("  tx queues: {}", options.tx_queues);
  std::println("  link_speeds: RTE_ETH_LINK_SPEED_AUTONEG ({})",
               conf.link_speeds);
  std::println("  rxmode.mq_mode: RTE_ETH_MQ_RX_NONE");
  if (options.mtu)
    std::println("  rxmode.mtu: {}", conf.rxmode.mtu);
  else
    std::println("  rxmode.mtu: PMD default");
  std::println("  rxmode.offloads: 0x{:016x}", conf.rxmode.offloads);
  std::println("  txmode.mq_mode: RTE_ETH_MQ_TX_NONE");
  std::println("  txmode.offloads: 0x{:016x}", conf.txmode.offloads);
  std::println("  action: {}",
               options.yes ? "call rte_eth_dev_configure" : "dry run only");
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

  bool configured = false;
  std::uint16_t configured_port = RTE_MAX_ETHPORTS;

  try {
    Options options = parse_app_options(original_args, first_app_arg);
    std::uint16_t port_id = resolve_port(options);
    configured_port = port_id;

    rte_eth_dev_info before = get_dev_info(port_id);
    validate_requested_config(options, before);
    rte_eth_conf conf = make_eth_conf(options);

    std::println("DPDK ethdev configure sample");
    std::println("  version: {}", rte_version());
    std::println("  EAL consumed argv entries: {}", eal_argc);
    std::println("  IOVA mode: {}", iova_mode_name(rte_eal_iova_mode()));
    print_eal_summary(device_args, no_pci);
    print_port_identity(port_id, before);
    print_queue_state("Before configure", before);
    print_config_plan(options, conf);

    if (!options.yes) {
      std::println("Pass --yes to perform rte_eth_dev_configure().");
    } else {
      int ret = rte_eth_dev_configure(port_id, options.rx_queues,
                                      options.tx_queues, &conf);
      if (ret != 0)
        throw std::runtime_error("rte_eth_dev_configure failed: " +
                                 dpdk_error(ret));
      configured = true;
      std::println("rte_eth_dev_configure returned success");

      rte_eth_dev_info after = get_dev_info(port_id);
      print_queue_state("After configure", after);
      std::println("Did not set up queues, start the device, or transmit "
                   "packets.");
    }

    if (configured) {
      int close_ret = rte_eth_dev_close(configured_port);
      if (close_ret != 0) {
        std::println(std::cerr, "rte_eth_dev_close failed: {}",
                     dpdk_error(close_ret));
        rte_eal_cleanup();
        return 1;
      }
      configured = false;
      std::println("Closed configured ethdev port before EAL cleanup");
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
    if (configured) {
      int close_ret = rte_eth_dev_close(configured_port);
      if (close_ret != 0)
        std::println(std::cerr, "rte_eth_dev_close after error failed: {}",
                     dpdk_error(close_ret));
    }
    rte_eal_cleanup();
    return 1;
  }
}
