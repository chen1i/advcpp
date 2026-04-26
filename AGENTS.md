# Repository Instructions

This repo is a C++ learning project with two independent tracks:

- `io_uring/`: C++20 `io_uring` exercises, from basic file I/O to echo servers, coroutines, registered fds, and multithreaded rings.
- `userspace_drivers/`: C++23 userspace PCI/device-driver exercises using Linux sysfs and MMIO.

## Build And Test

- Build all `io_uring` exercises with Make:
  `cd io_uring && make`
- Build `io_uring` with CMake:
  `cmake -S io_uring -B io_uring/build && cmake --build io_uring/build`
- Build userspace driver exercises:
  `cmake -S userspace_drivers -B userspace_drivers/build && cmake --build userspace_drivers/build`
- Basic smoke test:
  `cd io_uring && ./01_basic_read sample.txt`
- Echo-server smoke test:
  start a server such as `./07_echo_server 9000`, then run
  `./08_echo_bench -n 100 -r 100 -p 9000`.

## Coding Style

- Preserve the tutorial style: each exercise should explain its goal, background, and new concepts.
- Prefer small, self-contained examples over framework-style abstractions.
- Use RAII for owned resources, `std::system_error` for syscall/liburing failures, and standard containers/views where they clarify lifetime and size.
- Keep `io_uring` code C++20-compatible. Keep `userspace_drivers` code C++23-compatible.
- Follow existing formatting in the touched file. Avoid broad reformatting.
- When adding an exercise, update the nearest build file and README/table if applicable.

## Safety Notes

- Do not run `sudo`, change kernel/sysfs driver bindings, write MMIO registers, or issue destructive commands unless the user explicitly asks.
- Treat `/sys/bus/pci/devices` as real host hardware. Read-only enumeration/config inspection is usually safe; MMIO access needs an explicit target device and caution.
- Servers bind to test ports and may run indefinitely. Stop any server processes you start during verification.
- Do not edit generated build artifacts under `build/` or cache directories.
