#pragma once

// Thin RAII wrappers around POSIX file descriptors and io_uring rings.
//
// The io_uring kernel API is a C interface (struct io_uring, SQE, CQE).
// Wrapping it in thin RAII types is the standard C++ approach — you keep
// the low-level control while getting automatic resource management.

#include <liburing.h>

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>
#include <utility>

// ─── RAII wrapper for a POSIX file descriptor ───────────────────────
//
// Owns the fd.  Closes it in the destructor.  Non-copyable, movable.
// This pattern works for any integer-handle resource (fd, socket,
// epoll fd, timerfd, ...).

class FileDescriptor {
public:
    explicit FileDescriptor(const char* path, int flags, mode_t mode = 0)
        : fd_(open(path, flags, mode))
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

    // Block until at least one CQE is ready, then return how many
    // are available.  Caller should use peek()/seen() to drain them.
    unsigned wait_batch()
    {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_wait_cqe");
        return io_uring_cq_ready(&ring_);
    }

    // Submit all pending SQEs AND wait for at least min_complete CQEs
    // in a single syscall.  Returns how many CQEs are ready.
    unsigned submit_and_wait(unsigned min_complete = 1)
    {
        int ret = io_uring_submit_and_wait(&ring_, min_complete);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_submit_and_wait");
        return io_uring_cq_ready(&ring_);
    }

    // Peek at the head CQE without blocking.  Returns nullptr if empty.
    io_uring_cqe* peek()
    {
        io_uring_cqe* cqe = nullptr;
        io_uring_peek_cqe(&ring_, &cqe);
        return cqe;
    }

    // Advance the CQ head by count entries (batch version of seen).
    void advance(unsigned count) { io_uring_cq_advance(&ring_, count); }

    // Advance the CQ head — must call after processing a CQE.
    void seen(io_uring_cqe* cqe) { io_uring_cqe_seen(&ring_, cqe); }

    // Raw access for registration APIs (register_buffers, etc.)
    io_uring* raw() { return &ring_; }

private:
    io_uring ring_{};
};
