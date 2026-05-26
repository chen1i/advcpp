// Exercise 02: DPDK PCI probe visibility
//
// Goal: let EAL probe PCI devices, then print the ethdev ports that DPDK
// created and the generic rte_device metadata behind each port.
//
// This sample does not configure RX/TX queues and does not send packets. It is
// the next safe step after environment initialization: prove that an allowlisted
// PCI BDF becomes a DPDK ethdev port.

#include <array>
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
#include <rte_version.h>

struct Options {
  bool dump_devargs = false;
  std::vector<std::string> expect_ports;
};

struct EalDeviceArg {
  std::string option;
  std::string value;
};

static void usage(const char *argv0) {
  std::println(
      std::cerr,
      "Usage:\n"
      "  {} [EAL options] [--] [--expect-port BDF] [--dump-devargs]\n\n"
      "Examples:\n"
      "  {} -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- "
      "--expect-port 0000:c1:00.6\n"
      "  {} -l 0 -n 4 --no-pci\n\n"
      "Notes:\n"
      "  Put PCI allow/block options before -- because they are EAL options.\n"
      "  Use -a/--allow to keep the probe scoped to one target BDF.",
      argv0, argv0, argv0);
}

static int first_app_option_index(const std::vector<std::string> &args) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--")
      return static_cast<int>(i + 1);
  }
  return static_cast<int>(args.size());
}

static Options parse_app_options(const std::vector<std::string> &args,
                                 int first_app_arg) {
  Options options;

  for (std::size_t i = static_cast<std::size_t>(first_app_arg);
       i < args.size(); ++i) {
    std::string_view arg = args[i];
    if (arg == "--") {
      continue;
    } else if (arg == "--dump-devargs") {
      options.dump_devargs = true;
    } else if (arg == "--expect-port") {
      if (i + 1 >= args.size())
        throw std::runtime_error("--expect-port requires a BDF or device name");
      options.expect_ports.push_back(args[++i]);
    } else if (arg.starts_with("--expect-port=")) {
      options.expect_ports.emplace_back(arg.substr(14));
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

    if (arg.starts_with("--allow=")) {
      found.push_back({"--allow", std::string(arg.substr(8))});
    } else if (arg.starts_with("--block=")) {
      found.push_back({"--block", std::string(arg.substr(8))});
    } else if (arg.starts_with("-a") && arg.size() > 2) {
      found.push_back({"-a", std::string(arg.substr(2))});
    } else if (arg.starts_with("-b") && arg.size() > 2) {
      found.push_back({"-b", std::string(arg.substr(2))});
    }
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

static const char *policy_name(rte_dev_policy policy) {
  switch (policy) {
  case RTE_DEV_ALLOWED:
    return "allow";
  case RTE_DEV_BLOCKED:
    return "block";
  default:
    return "unknown";
  }
}

static const char *safe_cstr(const char *text) { return text ? text : ""; }

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

static void print_eal_device_arg_summary(
    const std::vector<EalDeviceArg> &device_args, bool no_pci) {
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

static void print_registered_devargs(bool dump_raw) {
  std::println("Registered EAL devargs:");

  const rte_devargs *devargs = nullptr;
  unsigned count = 0;
  while ((devargs = rte_devargs_next(nullptr, devargs)) != nullptr) {
    ++count;
    std::println("  {}:", count);
    std::println("    name: {}", devargs->name);
    std::println("    policy: {}", policy_name(devargs->policy));
    std::println("    bus: {}",
                 devargs->bus ? safe_cstr(rte_bus_name(devargs->bus)) : "");
    std::println("    bus_str: {}", safe_cstr(devargs->bus_str));
    std::println("    driver args: {}", safe_cstr(devargs->args));
    if (devargs->data)
      std::println("    raw: {}", devargs->data);
  }

  if (count == 0)
    std::println("  <none>");

  if (dump_raw) {
    std::println("Raw rte_devargs_dump:");
    rte_devargs_dump(stdout);
  }
}

static void print_generic_device(const rte_device *device) {
  if (!device) {
    std::println("    rte_device: <none>");
    return;
  }

  const rte_bus *bus = rte_dev_bus(device);
  const rte_driver *driver = rte_dev_driver(device);
  const rte_devargs *devargs = rte_dev_devargs(device);

  std::println("    rte_device:");
  std::println("      name: {}", safe_cstr(rte_dev_name(device)));
  std::println("      bus: {}", bus ? safe_cstr(rte_bus_name(bus)) : "");
  std::println("      bus_info: {}", safe_cstr(rte_dev_bus_info(device)));
  std::println("      driver: {}",
               driver ? safe_cstr(rte_driver_name(driver)) : "");
  std::println("      numa_node: {}", rte_dev_numa_node(device));
  std::println("      probed: {}", rte_dev_is_probed(device) ? "yes" : "no");
  if (devargs) {
    std::println("      devargs name: {}", devargs->name);
    std::println("      devargs policy: {}", policy_name(devargs->policy));
    std::println("      devargs args: {}", safe_cstr(devargs->args));
  } else {
    std::println("      devargs: <none>");
  }
}

static void print_ethdev_summary() {
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

    std::array<char, RTE_ETH_NAME_MAX_LEN> port_name{};
    ret = rte_eth_dev_get_name_by_port(port_id, port_name.data());
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

    std::println("  port {}:", port_id);
    std::println("    name: {}", port_name.data());
    std::println("    driver: {}", safe_cstr(info.driver_name));
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
    print_generic_device(info.device);
  }

  if (port_count == 0) {
    std::println("  note: no ethdev ports were created");
    std::println("  common causes: --no-pci was used, the BDF was not "
                 "allowlisted, the device is not bound to vfio/uio, or the "
                 "needed PMD is missing");
  }
}

static void print_expected_ports(const std::vector<std::string> &names) {
  if (names.empty())
    return;

  std::println("Expected port lookup:");
  for (const std::string &name : names) {
    std::uint16_t port_id = RTE_MAX_ETHPORTS;
    int ret = rte_eth_dev_get_port_by_name(name.c_str(), &port_id);
    if (ret == 0) {
      std::println("  {} -> port {}", name, port_id);
    } else {
      std::println("  {} -> not found ({})", name, rte_strerror(-ret));
    }
  }
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

    std::println("DPDK PCI probe check");
    std::println("  version: {}", rte_version());
    std::println("  EAL consumed argv entries: {}", eal_argc);
    std::println("  IOVA mode: {}", iova_mode_name(rte_eal_iova_mode()));

    print_eal_device_arg_summary(device_args, no_pci);
    print_registered_devargs(options.dump_devargs);
    print_ethdev_summary();
    print_expected_ports(options.expect_ports);

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
