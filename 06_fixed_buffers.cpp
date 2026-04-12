// Exercise 06: Fixed (Registered) Buffers
//
// Goal: speed up the pipelined copy from exercise 05 by registering
// our I/O buffers with the kernel once, up front.
//
// Why this helps
// ──────────────
// Every normal read/write through io_uring requires the kernel to:
//   1. Walk the page tables to find the physical pages behind the buffer
//   2. Pin those pages so they aren't swapped during DMA
//   3. Unpin them after the I/O completes
//
// With io_uring_register_buffers(), the kernel does this ONCE at
// registration time and caches the result.  Subsequent I/O with
// io_uring_prep_read_fixed / io_uring_prep_write_fixed skips
// the per-I/O page pinning entirely.
//
// The savings are most visible with many small I/Os — each one
// saves a page-table walk.  For large sequential I/O (like our
// 64K-chunk copy) the speedup is modest, but the pattern is
// essential for high-IOPS workloads (databases, storage engines).
//
// New concepts
// ────────────
// - io_uring_register_buffers / io_uring_unregister_buffers
// - io_uring_prep_read_fixed / io_uring_prep_write_fixed
// - Buffer index (buf_index) — identifies which registered buffer to use
// - struct iovec — the kernel's scatter/gather descriptor

#include "io_uring_utils.hpp"

#include <sys/stat.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
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

        // ── Allocate and register two buffers ───────────────
        //
        // The kernel needs an array of iovec describing each buffer.
        // After registration, we refer to buffers by index (0 or 1).

        alignas(4096) std::array<char, BUF_SIZE> buf[2]{};

        std::array<struct iovec, 2> iovs = {{
            { buf[0].data(), buf[0].size() },
            { buf[1].data(), buf[1].size() },
        }};

        int ret = io_uring_register_buffers(ring.raw(), iovs.data(), iovs.size());
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_register_buffers");

        off_t read_off  = 0;
        off_t write_off = 0;
        off_t file_size = st.st_size;
        int   cur       = 0;   // buffer index for next read

        // ── Kick off the first read (fixed buffer) ──────────
        {
            auto chunk = static_cast<unsigned>(
                std::min<off_t>(file_size - read_off, BUF_SIZE));
            auto* sqe = ring.get_sqe();
            io_uring_prep_read_fixed(sqe, src.get(),
                                     buf[cur].data(), chunk, read_off,
                                     /*buf_index=*/cur);
            io_uring_sqe_set_data64(sqe, TAG_READ);
            ring.submit();
            read_off += chunk;
        }

        bool read_done = (read_off >= file_size);
        bool has_pending_write = false;
        int  bytes_read = 0;

        // ── Pipeline loop (same structure as ex 05) ─────────

        while (true) {
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
                break;

            int filled = cur;
            cur ^= 1;

            // Write from the filled buffer (fixed)
            auto* write_sqe = ring.get_sqe();
            io_uring_prep_write_fixed(write_sqe, dst.get(),
                                      buf[filled].data(), bytes_read,
                                      write_off, /*buf_index=*/filled);
            io_uring_sqe_set_data64(write_sqe, TAG_WRITE);

            // Read into the other buffer (fixed), if not EOF
            bool more_reads = !read_done;
            if (more_reads) {
                auto chunk = static_cast<unsigned>(
                    std::min<off_t>(file_size - read_off, BUF_SIZE));
                auto* read_sqe = ring.get_sqe();
                io_uring_prep_read_fixed(read_sqe, src.get(),
                                         buf[cur].data(), chunk, read_off,
                                         /*buf_index=*/cur);
                io_uring_sqe_set_data64(read_sqe, TAG_READ);
                read_off += chunk;
                if (read_off >= file_size)
                    read_done = true;
            }

            ring.submit();

            write_off += bytes_read;
            has_pending_write = true;

            if (!more_reads) {
                auto* cqe = ring.wait();
                if (cqe->res < 0)
                    throw std::system_error(-cqe->res,
                        std::system_category(), "write");
                ring.seen(cqe);
                break;
            }
        }

        // Unregister buffers (optional — Ring destructor tears
        // down the ring which implicitly unregisters, but being
        // explicit is good practice).
        io_uring_unregister_buffers(ring.raw());

        std::cout << "Copied " << write_off << " bytes: "
                  << argv[1] << " -> " << argv[2] << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
