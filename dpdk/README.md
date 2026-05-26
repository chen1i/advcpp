# DPDK Study Journey

This track starts from the runtime environment before touching packet I/O.
DPDK applications are usually blocked first by hugepages, IOMMU/VFIO, device
binding, or missing PMDs, not by C++ code.

## Exercises

| # | Exercise | What you learn |
|---|----------|----------------|
| 01 | [Environment Check](01_env_check.cpp) | Initialize EAL, inspect lcores, hugepages, VFIO, IOVA mode, and visible ethdev ports |

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

## Static DPDK Binary

When DPDK static archives such as `librte_ethdev.a` are installed, the build
also creates `01_env_check_static`. This target links a small required subset of
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

cmake --build dpdk/build-static --target 01_env_check_static
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
