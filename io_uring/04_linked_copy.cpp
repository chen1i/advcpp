// Exercise 04: Linked SQEs — Kernel-chained File Copy
//
// Goal: improve the exercise-03 copy loop by linking each read→write
// pair so the kernel executes them back-to-back without returning to
// userspace between the two.
//
// Exercise 03 pattern (2 round-trips per chunk):
//
//   user: submit read  ──> kernel: read  ──> user: submit write ──> kernel: write
//
// This exercise (1 round-trip per chunk):
//
//   user: submit [read ──LINK──> write]  ──> kernel: read, then write  ──> user
//
// New concepts
// ────────────
// - IOSQE_IO_LINK flag — makes the next SQE wait for this one
// - Linked chain semantics:
//     • If a linked SQE fails OR does a short read/write,
//       the rest of the chain is cancelled (CQE with -ECANCELED)
//     • Each SQE in the chain still produces its own CQE
// - fstat to get file size — needed because we must request exact
//   byte counts to avoid short reads that break the link chain
//
// Limitation
// ──────────
// Read and write use the SAME buffer, which is safe here because
// LINK guarantees the read completes before the write starts.
// For true pipelining (read chunk N+1 while writing chunk N),
// you'd need double-buffering — a topic for a later exercise.

#include "io_uring_utils.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <src> <dst>\n";
        return 1;
    }

    try {
        FileDescriptor src(argv[1], O_RDONLY);
        FileDescriptor dst(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);

        // Get file size so we can request exact byte counts.
        // IOSQE_IO_LINK breaks the chain on short reads, so we
        // must never ask for more bytes than are available.
        struct stat st{};
        if (fstat(src.get(), &st) < 0)
            throw std::system_error(errno, std::system_category(), "fstat");
        off_t remaining = st.st_size;

        Ring ring(2);

        alignas(4096) std::array<char, 64 * 1024> buf{};
        off_t offset = 0;

        while (remaining > 0) {
            auto chunk = static_cast<unsigned>(
                std::min<off_t>(remaining, buf.size()));

            // ── Prepare a linked read → write pair ──────────
            //
            // The IOSQE_IO_LINK flag on the read SQE tells the
            // kernel: "don't start the NEXT SQE until this one
            // completes successfully."

            auto* read_sqe = ring.get_sqe();
            io_uring_prep_read(read_sqe, src.get(),
                               buf.data(), chunk, offset);
            read_sqe->flags |= IOSQE_IO_LINK;

            auto* write_sqe = ring.get_sqe();
            io_uring_prep_write(write_sqe, dst.get(),
                                buf.data(), chunk, offset);
            // write_sqe has no LINK flag — end of the chain

            // ── One submit for both ─────────────────────────
            ring.submit();

            // ── Reap the read CQE ──────────────────────────
            auto* read_cqe = ring.wait();
            if (read_cqe->res < 0)
                throw std::system_error(-read_cqe->res,
                    std::system_category(), "read");
            ring.seen(read_cqe);

            // ── Reap the write CQE ─────────────────────────
            auto* write_cqe = ring.wait();
            if (write_cqe->res < 0)
                throw std::system_error(-write_cqe->res,
                    std::system_category(), "write");
            ring.seen(write_cqe);

            offset    += chunk;
            remaining -= chunk;
        }

        std::cout << "Copied " << offset << " bytes: "
                  << argv[1] << " -> " << argv[2] << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
