// Exercise 18: Registered File Descriptors + Direct Accept
//
// Goal: eliminate the kernel's fd table lookup on every I/O by
// pre-registering a file descriptor table with io_uring.
//
// The problem
// ───────────
// Every recv/send/splice call does: fd number → process fd table
// → struct file*.  Under heavy I/O, this table lookup (which takes
// the file table lock) becomes a measurable overhead.
//
// The fix: io_uring_register_files
// ─────────────────────────────────
// Register a fixed array of fd slots with the kernel.  io_uring
// caches the struct file* pointers.  Subsequent I/O using
// IOSQE_FIXED_FILE skips the table lookup entirely.
//
// Direct accept
// ─────────────
// Normally, accept() allocates a new fd in the process fd table.
// With "direct" accept (IORING_FILE_INDEX_ALLOC), the kernel
// allocates the new socket directly into a registered fd slot —
// no process fd table entry at all.  The socket exists ONLY in
// the io_uring registered file table.
//
// This means:
// - accept:  no fd table insertion
// - recv/send: no fd table lookup (IOSQE_FIXED_FILE)
// - close:  use io_uring_prep_close_direct
//
// The fd never touches the process fd table.  For servers with
// thousands of connections, this reduces lock contention and
// cache misses on the fd table.
//
// New concepts
// ────────────
// - io_uring_register_files      — register fd slots
// - IORING_FILE_INDEX_ALLOC      — kernel picks a free slot
// - IOSQE_FIXED_FILE             — use registered fd, not process fd
// - io_uring_prep_close_direct   — close a registered fd slot
// - Fixed file index in SQE      — sqe->fd is the slot index, not an fd

#include "echo_common.hpp"

#include <array>
#include <coroutine>

static constexpr unsigned MAX_FILES = 4096;

// ─── Task + IoRequest ───────────────────────────────────────────────

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never  initial_suspend() { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

struct IoRequest {
    std::coroutine_handle<> handle;
    int result = 0;
};

// ─── Awaitables (fixed-file versions) ───────────────────────────────

// Direct accept: kernel allocates the new socket in a registered
// fd slot and returns the slot index in cqe->res.
struct AsyncDirectAccept : IoRequest {
    Ring& ring; int listen_fd;
    sockaddr_in addr{}; socklen_t len = sizeof(addr);

    AsyncDirectAccept(Ring& r, int lfd) : ring(r), listen_fd(lfd) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept_direct(sqe, listen_fd,
            reinterpret_cast<sockaddr*>(&addr), &len, 0,
            IORING_FILE_INDEX_ALLOC);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

// Recv using a registered fd slot (IOSQE_FIXED_FILE)
struct AsyncRecvFixed : IoRequest {
    Ring& ring; int slot; char* buf; unsigned len;

    AsyncRecvFixed(Ring& r, int s, char* b, unsigned l)
        : ring(r), slot(s), buf(b), len(l) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_recv(sqe, slot, buf, len, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

// Send using a registered fd slot
struct AsyncSendFixed : IoRequest {
    Ring& ring; int slot; const char* buf; unsigned len;

    AsyncSendFixed(Ring& r, int s, const char* b, unsigned l)
        : ring(r), slot(s), buf(b), len(l) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_send(sqe, slot, buf, len, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

// Close a registered fd slot
struct AsyncCloseFixed : IoRequest {
    Ring& ring; int slot;

    AsyncCloseFixed(Ring& r, int s) : ring(r), slot(s) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_close_direct(sqe, slot);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

// ─── Per-client coroutine ───────────────────────────────────────────
//
// Note: 'slot' is a registered file index, NOT a process fd.

Task client_handler(Ring& ring, int slot)
{
    std::array<char, 4096> buf{};

    for (;;) {
        AsyncRecvFixed recv_op(ring, slot, buf.data(), buf.size());
        int n = co_await recv_op;

        if (n <= 0) {
            if (n < 0 && n != -ECONNRESET)
                std::cerr << "recv (slot " << slot << "): "
                          << strerror(-n) << "\n";
            else if (n == 0)
                std::cout << "Client disconnected (slot " << slot << ")\n";
            break;
        }

        std::cout << "slot " << slot << ": "
                  << std::string_view(buf.data(), n);

        AsyncSendFixed send_op(ring, slot, buf.data(), n);
        int sent = co_await send_op;
        if (sent <= 0) break;
    }

    AsyncCloseFixed close_op(ring, slot);
    co_await close_op;
}

// ─── Accept loop ────────────────────────────────────────────────────
//
// Multishot accept keeps producing CQEs from a single SQE.
// We re-submit only if it terminates (error / no MORE flag).

Task accept_loop(Ring& ring, int listen_fd)
{
    for (;;) {
        AsyncDirectAccept accept_op(ring, listen_fd);
        int slot = co_await accept_op;

        if (slot < 0) {
            std::cerr << "accept: " << strerror(-slot) << "\n";
            continue;
        }

        std::cout << "Client connected (slot " << slot << ")\n";
        client_handler(ring, slot);
    }
}

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::ios_base::sync_with_stdio(false);
    std::cout.tie(nullptr);

    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port
                  << " (registered fds, direct accept)\n";

        Ring ring(4096, IORING_SETUP_SINGLE_ISSUER
                      | IORING_SETUP_DEFER_TASKRUN);

        // ── Register file descriptor slots ──────────────────
        //
        // Fill all slots with -1 (empty).  The kernel will
        // populate slots via direct accept.
        std::array<int, MAX_FILES> fds;
        fds.fill(-1);

        // Slot 0: the listen socket (so accept can use it)
        fds[0] = listen_fd;

        int ret = io_uring_register_files(ring.raw(),
                                          fds.data(), fds.size());
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_register_files");

        accept_loop(ring, listen_fd);

        for (;;) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                auto* req = static_cast<IoRequest*>(
                    io_uring_cqe_get_data(cqe));
                int res = cqe->res;
                ring.seen(cqe);

                req->result = res;
                req->handle.resume();
            }

            std::cout.flush();
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
