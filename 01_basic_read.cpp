// Exercise 01: Basic io_uring Read — C++ edition
//
// Same 8-step workflow, but now with C++ idioms:
//
//   - RAII wrappers      → no manual close()/queue_exit(), no leak on error
//   - std::system_error  → errors carry context, no fprintf scattered around
//   - std::array         → sized buffer on the stack
//   - std::string_view   → zero-copy view of the bytes we read
//
// The io_uring kernel API is a C interface (struct io_uring, SQE, CQE).
// Wrapping it in thin RAII types is the standard C++ approach — you keep
// the low-level control while getting automatic resource management.

#include <liburing.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

// ─── RAII wrapper for a POSIX file descriptor ───────────────────────
//
// Owns the fd.  Closes it in the destructor.  Non-copyable, movable.
// This pattern works for any integer-handle resource (fd, socket,
// epoll fd, timerfd, ...).

class FileDescriptor {
public:
    explicit FileDescriptor(const char* path, int flags)
        : fd_(open(path, flags))
    {
        if (fd_ < 0)
            throw std::system_error(errno, std::system_category(), path);
    }

    ~FileDescriptor() { if (fd_ >= 0) close(fd_); }

    // movable
    FileDescriptor(FileDescriptor&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) close(fd_); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }

    // non-copyable
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

// ─── RAII wrapper for struct io_uring ───────────────────────────────
//
// Constructor inits the ring, destructor tears it down.
// Thin methods forward to the liburing helpers.

class Ring {
public:
    explicit Ring(unsigned queue_depth, unsigned flags = 0)
    {
        int ret = io_uring_queue_init(queue_depth, &ring_, flags);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
    }

    ~Ring() { io_uring_queue_exit(&ring_); }

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    // Get a free SQE slot, or throw if the SQ is full.
    io_uring_sqe* get_sqe()
    {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
            throw std::runtime_error("submission queue is full");
        return sqe;
    }

    // Push all pending SQEs to the kernel.  Returns count submitted.
    int submit()
    {
        int ret = io_uring_submit(&ring_);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_submit");
        return ret;
    }

    // Block until at least one CQE is ready.
    io_uring_cqe* wait()
    {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_wait_cqe");
        return cqe;
    }

    // Advance the CQ head — must call after processing a CQE.
    void seen(io_uring_cqe* cqe) { io_uring_cqe_seen(&ring_, cqe); }

private:
    io_uring ring_{};
};

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    const char* path = (argc > 1) ? argv[1] : "sample.txt";

    try {
        FileDescriptor file(path, O_RDONLY);              // RAII open
        Ring ring(/*queue_depth=*/4);                     // RAII ring init

        // Prepare a read SQE
        auto* sqe = ring.get_sqe();
        std::array<char, 4096> buf{};
        io_uring_prep_read(sqe, file.get(), buf.data(), buf.size(), /*offset=*/0);
        io_uring_sqe_set_data64(sqe, file.get());

        // Submit & wait
        int n = ring.submit();
        std::cout << "Submitted " << n << " SQE(s)\n";

        auto* cqe = ring.wait();

        if (cqe->res < 0) {
            throw std::system_error(-cqe->res, std::system_category(), "read");
        }

        // Zero-copy view over the filled portion of buf
        std::string_view result(buf.data(), cqe->res);
        std::cout << "Read " << result.size() << " bytes:\n" << result;

        ring.seen(cqe);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    // FileDescriptor and Ring destructors handle cleanup automatically —
    // even if an exception is thrown mid-way through.

    return 0;
}
