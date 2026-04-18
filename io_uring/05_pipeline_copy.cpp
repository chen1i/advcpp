// Exercise 05: Double-buffered Pipelined File Copy
//
// Goal: overlap read and write I/O by using two buffers.
// While the kernel writes chunk N, we've already submitted
// the read for chunk N+1 into the other buffer.
//
// Exercises 03/04 pattern (serial):
//
//   read₀ ──── write₀ ──── read₁ ──── write₁ ──── ...
//
// This exercise (pipelined):
//
//   read₀ ──┐
//            ├── write₀ + read₁ ──┐
//            │                     ├── write₁ + read₂ ──┐
//            │                     │                     ├── ...
//
// At steady state, the kernel works on a read and a write
// simultaneously — the SSD's read and write paths can overlap.
//
// New concepts
// ────────────
// - Double buffering — two buffers alternate roles (fill / drain)
// - Pipelining — submit the next read before reaping the current write
// - Managing multiple in-flight operations without linked SQEs
// - Using user_data tags to distinguish read vs write CQEs

#include "io_uring_utils.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

static constexpr std::size_t BUF_SIZE = 64 * 1024;
static constexpr __u64 TAG_READ  = 0;
static constexpr __u64 TAG_WRITE = 1;

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <src> <dst>\n";
        return 1;
    }

    try {
        FileDescriptor src(argv[1], O_RDONLY);
        FileDescriptor dst(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);

        struct stat st{};
        if (fstat(src.get(), &st) < 0)
            throw std::system_error(errno, std::system_category(), "fstat");

        Ring ring(4);

        // Two buffers that alternate: one is being written from
        // while the other is being read into.
        alignas(4096) std::array<char, BUF_SIZE> buf[2]{};

        off_t read_off  = 0;
        off_t write_off = 0;
        off_t file_size = st.st_size;
        int   cur       = 0;   // which buffer the next read fills

        // ── Kick off the first read ─────────────────────────
        {
            auto chunk = static_cast<unsigned>(
                std::min<off_t>(file_size - read_off, BUF_SIZE));
            auto* sqe = ring.get_sqe();
            io_uring_prep_read(sqe, src.get(), buf[cur].data(), chunk, read_off);
            io_uring_sqe_set_data64(sqe, TAG_READ);
            ring.submit();
            read_off += chunk;
        }

        bool read_done = (read_off >= file_size);

        // ── Main pipeline loop ──────────────────────────────
        //
        // Each iteration:
        //   1. Wait for the in-flight read to complete
        //   2. Submit a write for the data we just read
        //   3. Submit the next read into the OTHER buffer (if not EOF)
        //   4. Wait for the previous write to complete (if any)

        bool has_pending_write = false;
        int  bytes_read = 0;

        while (true) {
            // 1. Reap all pending CQEs (1 read + maybe 1 previous write).
            //    CQEs can arrive in ANY order, so we check user_data
            //    to tell them apart.

            int to_reap = 1 + (has_pending_write ? 1 : 0);
            bytes_read = 0;

            for (int i = 0; i < to_reap; ++i) {
                auto* cqe = ring.wait();

                if (cqe->user_data == TAG_READ) {
                    if (cqe->res < 0)
                        throw std::system_error(-cqe->res,
                            std::system_category(), "read");
                    bytes_read = cqe->res;
                } else {
                    if (cqe->res < 0)
                        throw std::system_error(-cqe->res,
                            std::system_category(), "write");
                }

                ring.seen(cqe);
            }

            if (bytes_read == 0)
                break;  // EOF

            int filled = cur;       // buffer that now has data
            cur ^= 1;              // swap to the other buffer

            // 2. Submit write of the filled buffer
            auto* write_sqe = ring.get_sqe();
            io_uring_prep_write(write_sqe, dst.get(),
                                buf[filled].data(), bytes_read, write_off);
            io_uring_sqe_set_data64(write_sqe, TAG_WRITE);

            // 3. Submit next read into the other buffer (if not EOF)
            bool more_reads = !read_done;
            if (more_reads) {
                auto chunk = static_cast<unsigned>(
                    std::min<off_t>(file_size - read_off, BUF_SIZE));
                auto* read_sqe = ring.get_sqe();
                io_uring_prep_read(read_sqe, src.get(),
                                   buf[cur].data(), chunk, read_off);
                io_uring_sqe_set_data64(read_sqe, TAG_READ);
                read_off += chunk;
                if (read_off >= file_size)
                    read_done = true;
            }

            ring.submit();

            write_off += bytes_read;
            has_pending_write = true;

            // No more reads — drain the final write and exit
            if (!more_reads) {
                auto* cqe = ring.wait();
                if (cqe->res < 0)
                    throw std::system_error(-cqe->res,
                        std::system_category(), "write");
                ring.seen(cqe);
                break;
            }
        }

        std::cout << "Copied " << write_off << " bytes: "
                  << argv[1] << " -> " << argv[2] << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
