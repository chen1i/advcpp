// Exercise 03: File Copy with io_uring
//
// Goal: copy a file by alternating read and write operations through
// the io_uring ring, processing one chunk at a time.
//
// New concepts
// ────────────
// - io_uring_prep_write — the write counterpart to prep_read
// - Offset tracking — advancing the file position manually
// - Loop until EOF — cqe->res == 0 means no more data
// - Short reads — cqe->res may be less than the buffer size;
//   we must write exactly the number of bytes we read
//
// Why not just use sendfile / copy_file_range?
// ─────────────────────────────────────────────
// Those are great for simple copies, but this pattern generalises:
// read from a socket, transform in userspace, write to disk — the
// same submit/reap loop works for any combination of I/O.

#include "io_uring_utils.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <src> <dst>\n";
        return 1;
    }

    try {
        FileDescriptor src(argv[1], O_RDONLY);
        FileDescriptor dst(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);

        Ring ring(2);

        alignas(4096) std::array<char, 64 * 1024> buf{};
        off_t offset = 0;

        for (;;) {
            // ── Submit a read ───────────────────────────────
            auto* sqe = ring.get_sqe();
            io_uring_prep_read(sqe, src.get(), buf.data(), buf.size(), offset);
            ring.submit();

            auto* cqe = ring.wait();
            if (cqe->res < 0)
                throw std::system_error(-cqe->res, std::system_category(), "read");

            int bytes_read = cqe->res;
            ring.seen(cqe);

            if (bytes_read == 0)
                break;  // EOF

            // ── Submit a write of exactly the bytes we read ─
            sqe = ring.get_sqe();
            io_uring_prep_write(sqe, dst.get(), buf.data(), bytes_read, offset);
            ring.submit();

            cqe = ring.wait();
            if (cqe->res < 0)
                throw std::system_error(-cqe->res, std::system_category(), "write");
            ring.seen(cqe);

            offset += bytes_read;
        }

        std::cout << "Copied " << offset << " bytes: "
                  << argv[1] << " -> " << argv[2] << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
