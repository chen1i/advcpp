# DPDK Study Journey

This track starts from the runtime environment before touching packet I/O.
DPDK applications are usually blocked first by hugepages, IOMMU/VFIO, device
binding, or missing PMDs, not by C++ code.

## Exercises

| # | Exercise | What you learn |
|---|----------|----------------|
| 01 | [Environment Check](01_env_check.cpp) | Initialize EAL, inspect lcores, hugepages, VFIO, IOVA mode, and visible ethdev ports |
| 02 | [PCI Probe Visibility](02_pci_probe.cpp) | Use EAL PCI allowlists, inspect registered devargs, and map probed ethdev ports back to their rte_device metadata |
| 03 | [Ethdev Info](03_ethdev_info.cpp) | Inspect queue limits, descriptor limits, offload capabilities, RSS capabilities, supported packet types, and link metadata |
| 04 | [Ethdev Configure](04_ethdev_configure.cpp) | Call `rte_eth_dev_configure()` with a minimal `rte_eth_conf`, verify configured queue counts, and close the port without queue setup or start |
| 05 | [Ethdev Queue Setup](05_ethdev_queue_setup.cpp) | Create an mbuf mempool, adjust descriptor counts, set up RX/TX queues, and close the port without starting packet I/O |

## Build

```bash
cmake -S dpdk -B dpdk/build
cmake --build dpdk/build
```

If CMake prints that `libdpdk.pc` is missing, install the DPDK development
package or point `PKG_CONFIG_PATH` at the directory containing `libdpdk.pc`.

## Run

DPDK EAL options go before the application `--` separator. Run the binary with
the permissions your DPDK setup requires, for example as root or with the
needed VFIO/hugepage access:

```bash
./dpdk/build/01_env_check -l 0-1 -n 4 --log-level=lib.eal:info -- --dump-memzones
```

For a first smoke test on a machine with DPDK installed:

```bash
./dpdk/build/01_env_check -l 0-1 -n 4
```

The sample does not configure queues or send packets. It only initializes EAL
and prints what DPDK can see.

To let DPDK probe one PCI device without touching queues, use sample 02 with an
EAL allowlist. Put `-a` before the application `--` separator:

```bash
./dpdk/build/02_pci_probe -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- \
  --expect-port 0000:c1:00.6
```

For the portable static binary on the target host:

```bash
env XDG_RUNTIME_DIR=/tmp ./02_pci_probe_static -l 0 -n 4 --no-huge \
  -a 0000:c1:00.6 -- --expect-port 0000:c1:00.6
```

If the target device is a virtio-net VF bound to `vfio-pci`, a successful probe
should show one available ethdev port, its PMD driver, MAC address, and the
backing `rte_device` name/bus/devargs. The sample still does not call
`rte_eth_dev_configure()` or set up RX/TX queues.

After sample 02 proves that the BDF becomes a DPDK port, sample 03 reads the
port capabilities that later configuration samples must obey:

```bash
env XDG_RUNTIME_DIR=/tmp ./03_ethdev_info_static -l 0 -n 4 --no-huge \
  -a 0000:c1:00.6 -- --port-name 0000:c1:00.6
```

This prints queue limits, descriptor alignment/min/max values, RX/TX offload
capability bitmaps, RSS capability information, supported packet types, and
basic link metadata. It still does not configure queues or start the device.

Sample 04 is the first state-changing ethdev call. The default invocation is a
dry run that validates the requested queue counts and prints the
`rte_eth_dev_configure()` plan:

```bash
env XDG_RUNTIME_DIR=/tmp ./04_ethdev_configure_static -l 0 -n 4 --no-huge \
  -a 0000:c1:00.6 -- --port-name 0000:c1:00.6
```

Add `--yes` to actually configure one RX queue and one TX queue, then close the
port before EAL cleanup:

```bash
env XDG_RUNTIME_DIR=/tmp ./04_ethdev_configure_static -l 0 -n 4 --no-huge \
  -a 0000:c1:00.6 -- --port-name 0000:c1:00.6 --yes
```

This still does not call `rte_eth_rx_queue_setup()`,
`rte_eth_tx_queue_setup()`, or `rte_eth_dev_start()`. If it succeeds, the
post-configure `rte_eth_dev_info_get()` output should show the configured RX/TX
queue counts changing from zero to the requested values.

Sample 05 adds the next two state changes: packet mbuf pool creation and RX/TX
queue setup. The dry run shows the exact configure, mempool, descriptor, and
queue setup plan:

```bash
./05_ethdev_queue_setup_static -l 0 -n 4 --no-huge \
  -a 0000:c1:00.6 -- --port-name 0000:c1:00.6
```

Add `--yes` to configure the port, create an `rte_pktmbuf_pool`, let the PMD
adjust requested descriptor counts, set up one RX queue and one TX queue, then
close the port and free the mempool:

```bash
./05_ethdev_queue_setup_static -l 0 -n 4 --no-huge \
  -a 0000:c1:00.6 -- --port-name 0000:c1:00.6 --yes
```

The default memory socket is `SOCKET_ID_ANY` because this tutorial often runs
with `-l 0` even when the target PCI device is on a different NUMA node. Use
`--socket port` when you want queue and mbuf memory allocated on the device's
reported socket. This sample still does not call `rte_eth_dev_start()`, so no
packet can be received or transmitted yet.

## Static DPDK Binary

When DPDK static archives such as `librte_ethdev.a` are installed, the build
also creates `*_static` binaries. These targets link a small required subset of
DPDK static archives directly instead of using the full `pkg-config --static
libdpdk` link line. That keeps the smoke-test binary portable enough for hosts
that do not have matching `librte_*.so.<abi>` libraries installed, and it avoids
pulling optional PMD dependencies such as libbsd, libfdt, libarchive, OpenSSL,
pcap, and jansson into this first sample.

Static DPDK PMDs and mempool ops are registered by constructors, so archives
that provide plugin-style objects sometimes need explicit linker nudges. The
build forces in the PCI bus, virtio-net PMD, and ring mempool ops used by these
early exercises.

The static target is a fully static ELF, matching the portability model used by
the `userspace_drivers/*_static` samples. It statically links the C++ runtime
and avoids a runtime dependency on the build host's glibc symbol versions:

```bash
./dpdk/build/01_env_check_static -l 0-1 -n 4 --no-pci --no-huge
./dpdk/build/02_pci_probe_static -l 0 -n 4 --no-huge -a 0000:c1:00.6 -- \
  --expect-port 0000:c1:00.6
```

Use this binary when the target machine does not have matching
`librte_*.so.<abi>` shared libraries installed.

If CMake says `*_static targets are disabled`, the local DPDK package only
installed shared libraries. Install the DPDK static libraries package, or build
DPDK from source with static libraries enabled, then re-run CMake.

For a separate static DPDK install under `/opt/dpdk-25.11-static`:

```bash
env PKG_CONFIG_PATH=/opt/dpdk-25.11-static/lib/pkgconfig \
  cmake -S dpdk -B dpdk/build-static

cmake --build dpdk/build-static
```

Check that the resulting binary is fully static:

```bash
file dpdk/build-static/01_env_check_static
readelf -d dpdk/build-static/01_env_check_static | grep NEEDED
```

The `file` output should say `statically linked`, and the `readelf` command
should print no `NEEDED` entries. This means the target host should not need
matching `librte_*.so`, `libstdc++.so`, `libbsd.so`, or newer glibc runtime
versions from the build host.

This first sample provides small compatibility shims for the DPDK static EAL
path, including `strlcpy`, newer glibc C23 parsing symbols, and the tiny subset
of libnuma calls needed by `--no-huge` environment probing.
