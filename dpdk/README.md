# DPDK Study Journey

This track starts from the runtime environment before touching packet I/O.
DPDK applications are usually blocked first by hugepages, IOMMU/VFIO, device
binding, or missing PMDs, not by C++ code.

## Exercises

| # | Exercise | What you learn |
|---|----------|----------------|
| 01 | [Environment Check](01_env_check.cpp) | Initialize EAL, inspect lcores, hugepages, VFIO, IOVA mode, and visible ethdev ports |
| 02 | [PCI Probe Visibility](02_pci_probe.cpp) | Use EAL PCI allowlists, inspect registered devargs, and map probed ethdev ports back to their rte_device metadata |

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

## Static DPDK Binary

When DPDK static archives such as `librte_ethdev.a` are installed, the build
also creates `*_static` binaries. These targets link a small required subset of
DPDK static archives directly instead of using the full `pkg-config --static
libdpdk` link line. That keeps the smoke-test binary portable enough for hosts
that do not have matching `librte_*.so.<abi>` libraries installed, and it avoids
pulling optional PMD dependencies such as libbsd, libfdt, libarchive, OpenSSL,
pcap, and jansson into this first sample.

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
