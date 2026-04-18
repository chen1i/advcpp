// Exercise 02: Batched Multi-file Read
//
// Goal: submit N read operations with a *single* io_uring_submit() call,
// then reap all N completions.
//
// Why this matters
// ────────────────
// With traditional synchronous I/O, reading 3 files means 3 syscalls
// that execute one-at-a-time:
//
//   read(fd_a) ──block──> done → read(fd_b) ──block──> done → read(fd_c) ...
//
// With io_uring, you queue all 3 SQEs, then make ONE submit() syscall.
// The kernel can issue all 3 reads in parallel (especially beneficial
// for network I/O or when files live on different devices):
//
//   submit([read_a, read_b, read_c])  ──>  kernel works on all 3  ──>  3 CQEs
//
// New concepts
// ────────────
// - Submitting multiple SQEs at once
// - Reaping multiple CQEs in a loop
// - Using user_data to identify which completion belongs to which file
// - Completions may arrive in ANY order (kernel decides)

#include "io_uring_utils.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// ─── Per-request context ────────────────────────────────────────────
//
// Each in-flight read needs its own buffer and metadata.
// We store these in a vector and pass the index as user_data,
// so we can look up the context when the CQE arrives.

struct ReadRequest {
    std::string             filename;
    FileDescriptor          fd;
    std::array<char, 4096>  buf{};

    ReadRequest(const char* path)
        : filename(path), fd(path, O_RDONLY) {}
};

// ─── main ───────────────────────────────────────────────────────────

int main()
{
    const std::array paths = {"a.txt", "b.txt", "c.txt"};

    try {
        // Open all files and allocate per-request buffers
        std::vector<ReadRequest> requests;
        requests.reserve(paths.size());
        for (auto* path : paths) {
            requests.emplace_back(path);
        }

        Ring ring(paths.size());

        // ── Queue one SQE per file ──────────────────────────────
        //
        // Nothing is sent to the kernel yet — get_sqe + prep_read
        // just fill slots in the userspace SQ ring buffer.

        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto* sqe = ring.get_sqe();
            auto& req = requests[i];

            io_uring_prep_read(sqe, req.fd.get(),
                               req.buf.data(), req.buf.size(),
                               /*offset=*/0);

            // Store the index so we can find this ReadRequest from the CQE
            io_uring_sqe_set_data64(sqe, i);
        }

        // ── Single submit for ALL reads ─────────────────────────
        //
        // One syscall dispatches 3 read operations to the kernel.
        // Compare: synchronous code would need 3 separate read() syscalls.

        int submitted = ring.submit();
        std::cout << "Submitted " << submitted << " SQE(s) in one syscall\n\n";

        // ── Reap all completions ────────────────────────────────
        //
        // We know exactly how many CQEs to expect (one per SQE).
        // IMPORTANT: completions may arrive in any order!
        // The kernel processes them however it sees fit.

        for (int i = 0; i < submitted; ++i) {
            auto* cqe = ring.wait();

            auto idx = cqe->user_data;  // the index we stored earlier
            auto& req = requests[idx];

            if (cqe->res < 0) {
                std::cerr << req.filename << ": "
                          << strerror(-cqe->res) << "\n";
            } else {
                std::string_view data(req.buf.data(), cqe->res);
                std::cout << "[CQE #" << i << "] "
                          << req.filename << " (" << cqe->res << " bytes): "
                          << data;
            }

            ring.seen(cqe);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
