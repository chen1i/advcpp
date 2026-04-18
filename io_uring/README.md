# io_uring Study Journey

A hands-on progression through Linux's `io_uring` async I/O interface,
from first syscall to production-grade patterns.  Every exercise builds,
runs, and benchmarks — no toy examples.

## The Exercises

### Part 1: File I/O Fundamentals (01–06)

| # | Exercise | What you learn |
|---|----------|----------------|
| 01 | [Basic Read](01_basic_read.cpp) | RAII wrappers for fd and ring, the 8-step SQE/CQE lifecycle |
| 02 | [Batch Read](02_batch_read.cpp) | Submit N reads in one syscall, reap completions in any order |
| 03 | [Serial Copy](03_file_copy.cpp) | Read/write loop, offset tracking, EOF detection |
| 04 | [Linked Copy](04_linked_copy.cpp) | `IOSQE_IO_LINK` chains read→write; short reads break chains |
| 05 | [Pipelined Copy](05_pipeline_copy.cpp) | Double buffering — read chunk N+1 while writing chunk N |
| 06 | [Fixed Buffers](06_fixed_buffers.cpp) | `io_uring_register_buffers` skips per-I/O page pinning |

**Key lesson:** Exercise 05 (pipelining) beat exercises 03 and 04 by ~25%
on a 1.5GB ISO copy.  Overlapping I/O matters more than reducing syscalls.

### Part 2: Networking & Server Architecture (07–11)

| # | Exercise | What you learn |
|---|----------|----------------|
| 07 | [Echo Server](07_echo_server.cpp) | Batched `submit_and_wait`, [epoll comparison](07_echo_server_epoll.cpp), [naive comparison](07_echo_server_uring_naive.cpp) |
| 08 | [Bench Client](08_echo_bench.cpp) | io_uring from the client side, removed Python bottleneck |
| 09 | [SQPOLL](09_echo_sqpoll.cpp) | Kernel polling thread — zero submit() syscalls |
| 10 | [Provided Buffers](10_provided_buffers.cpp) | Buffer rings, O(active) memory instead of O(clients) |
| 11 | [Multi-shot](11_multishot.cpp) | Multi-shot accept + recv, one SQE produces many CQEs |

**Key lesson:** Naive io_uring (submit per SQE) is *slower* than epoll.
Batching is everything — one `submit_and_wait()` for N operations is
what makes io_uring 2x faster than epoll.

### Benchmark Results (1M messages, 1000 clients)

| Server | msg/s | Technique |
|--------|------:|-----------|
| naive io_uring | 76K | submit() per SQE |
| epoll | 86K | traditional readiness model |
| SQPOLL | 101K | kernel polls SQ |
| Boost.Asio | 103K | io_uring backend, abstracted |
| splice | 131K | zero-copy (kernel only) |
| provided buffers | 142K | shared buffer pool |
| multishot | 151K | multi-shot accept/recv |
| batched + optimized | 163K | SINGLE_ISSUER + DEFER_TASKRUN |
| registered fds | 165K | fd table bypass |
| **coroutine** | **170K** | all optimizations + coroutines |

*Measured on 4-core machine, localhost, with server-side logging enabled.*

### Part 3: Coroutines & Clean Async Code (12–16)

| # | Exercise | What you learn |
|---|----------|----------------|
| 12 | [Coroutine Echo](12_coroutine_echo.cpp) | C++20 coroutines over io_uring — `co_await` replaces switch/callback |
| 13 | [File Server](13_file_server.cpp) | HTTP file server: disk + network I/O in one ring, one coroutine |
| 14 | [Timeouts](14_timeout.cpp) | `io_uring_prep_link_timeout`, `noop_coroutine` for the timeout CQE |
| 15 | [Graceful Shutdown](15_graceful_shutdown.cpp) | signalfd + `io_uring_prep_cancel_fd` + clean exit |
| 16 | [Boost.Asio](16_asio_echo.cpp) | Same server in ~80 lines, cross-platform, io_uring backend |

**Key lesson:** The coroutine echo server was the fastest (170K msg/s)
despite heap-allocated frames — no hash map lookups, no user_data
encoding, better cache behavior.

### Part 4: Advanced Techniques (17–19)

| # | Exercise | What you learn |
|---|----------|----------------|
| 17 | [Splice Echo](17_splice_echo.cpp) | Zero-copy: socket→pipe→socket, data never enters userspace |
| 18 | [Registered FDs](18_registered_fds.cpp) | Direct accept, `IOSQE_FIXED_FILE`, fd table bypass |
| 19 | [Multi-thread](19_multithread_echo.cpp) | Thread-per-ring, SPSC queue + eventfd, round-robin distribution |

**Key lesson:** Multi-threading lost to single-threaded on a 4-core
machine with a trivial workload.  Thread coordination costs more than
the echo work itself.  Multi-threading pays off with CPU-heavy per-request
work and many cores.

## Shared Code

| File | Purpose |
|------|---------|
| [io_uring_utils.hpp](io_uring_utils.hpp) | `FileDescriptor` + `Ring` RAII wrappers |
| [echo_common.hpp](echo_common.hpp) | Op encoding, `create_listener()` |
| [07_echo_common.hpp](07_echo_common.hpp) | Per-client-buffer `handle_cqe()` |
| [test_echo.py](test_echo.py) | Python stress-test client |

## io_uring Optimization Knobs Summary

| Layer | Technique | Effect |
|-------|-----------|--------|
| Submission | Batched `submit_and_wait` | Amortize syscall cost across N ops |
| Submission | `IORING_SETUP_SQPOLL` | Kernel thread polls SQ, no submit() |
| Submission | `IORING_SETUP_SINGLE_ISSUER` | No SQ locking (single thread) |
| CQE delivery | `IORING_SETUP_DEFER_TASKRUN` | Batch CQE delivery, fewer interrupts |
| Buffers | `io_uring_register_buffers` | Skip per-I/O page pinning |
| Buffers | Provided buffer rings | Kernel picks buffers, O(active) memory |
| File descriptors | `io_uring_register_files` | Skip fd table lookup |
| File descriptors | Direct accept | Socket never enters process fd table |
| SQE lifetime | Multi-shot accept/recv | One SQE, many CQEs |
| Chaining | `IOSQE_IO_LINK` | Kernel-side sequencing |
| Data path | `splice` | Zero-copy forwarding |

## Build

```bash
make            # build all exercises
make clean      # remove binaries
```

Requires: Linux 6.0+, liburing, g++ with C++20 support.
Exercise 16 additionally requires Boost (for Boost.Asio).
