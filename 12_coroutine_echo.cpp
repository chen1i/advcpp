// Exercise 12: C++20 Coroutines + io_uring
//
// Goal: replace the manual state machine (switch on Op, encode/decode
// user_data) with coroutines.  Each client connection becomes a
// simple sequential function:
//
//   Task client_handler(Ring& ring, int fd) {
//       while (true) {
//           int n = co_await async_recv(ring, fd, buf);
//           if (n <= 0) break;
//           co_await async_send(ring, fd, buf, n);
//       }
//       close(fd);
//   }
//
// The io_uring event loop resumes the right coroutine when each
// I/O completes — no callbacks, no Op enum, no user_data packing.
//
// How it works
// ────────────
// 1. An Awaitable prepares an SQE and stores a pointer to itself
//    (an IoRequest) in the SQE's user_data, then suspends
// 2. The event loop reaps CQEs, recovers the IoRequest pointer,
//    writes cqe->res into it, and resumes the coroutine
// 3. The resumed coroutine reads the result via await_resume()
//
// New C++ concepts
// ─────────────────
// - coroutine_handle<>      — type-erased handle to a suspended coroutine
// - co_await                — suspend until the awaitable completes
// - promise_type            — controls coroutine lifecycle
// - await_suspend/resume    — the awaitable protocol
//
// This is a minimal coroutine integration — no allocator, no
// scheduler, no cancellation.  Production frameworks (libunifex,
// Asio) add those layers on top of the same primitives.

#include "echo_common.hpp"

#include <array>
#include <coroutine>

// ─── Task: a fire-and-forget coroutine ──────────────────────────────
//
// The simplest possible coroutine return type.  The coroutine runs
// until it co_awaits, at which point the event loop takes over.
// When the coroutine finishes, it destroys itself.

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never  initial_suspend() { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// ─── IoRequest: base for all awaitables ─────────────────────────────
//
// Stored in SQE user_data.  The event loop casts back to IoRequest*,
// writes the CQE result, and resumes the coroutine.

struct IoRequest {
    std::coroutine_handle<> handle;
    int result = 0;
};

// ─── Awaitables ─────────────────────────────────────────────────────

struct AsyncAccept : IoRequest {
    Ring& ring;
    int   listen_fd;
    sockaddr_in  addr{};
    socklen_t    len = sizeof(addr);

    AsyncAccept(Ring& r, int lfd) : ring(r), listen_fd(lfd) {}

    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&addr), &len, 0);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }

    int await_resume() { return result; }
};

struct AsyncRecv : IoRequest {
    Ring& ring;
    int   fd;
    char* buf;
    unsigned len;

    AsyncRecv(Ring& r, int f, char* b, unsigned l)
        : ring(r), fd(f), buf(b), len(l) {}

    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }

    int await_resume() { return result; }
};

struct AsyncSend : IoRequest {
    Ring& ring;
    int   fd;
    const char* buf;
    unsigned len;

    AsyncSend(Ring& r, int f, const char* b, unsigned l)
        : ring(r), fd(f), buf(b), len(l) {}

    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_send(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }

    int await_resume() { return result; }
};

// ─── Per-client coroutine ───────────────────────────────────────────
//
// This reads like synchronous blocking code, but each co_await
// suspends the coroutine and returns control to the event loop.

Task client_handler(Ring& ring, int fd)
{
    std::array<char, 4096> buf{};

    for (;;) {
        AsyncRecv recv_op(ring, fd, buf.data(), buf.size());
        int n = co_await recv_op;

        if (n <= 0) {
            if (n < 0)
                std::cerr << "recv (fd " << fd << "): "
                          << strerror(-n) << "\n";
            else
                std::cout << "Client disconnected (fd " << fd << ")\n";
            break;
        }

        std::cout << "fd " << fd << ": "
                  << std::string_view(buf.data(), n);

        AsyncSend send_op(ring, fd, buf.data(), n);
        int sent = co_await send_op;

        if (sent < 0) {
            std::cerr << "send (fd " << fd << "): "
                      << strerror(-sent) << "\n";
            break;
        }
    }

    close(fd);
}

// ─── Accept loop coroutine ──────────────────────────────────────────

Task accept_loop(Ring& ring, int listen_fd)
{
    for (;;) {
        AsyncAccept accept_op(ring, listen_fd);
        int client_fd = co_await accept_op;

        if (client_fd < 0) {
            std::cerr << "accept: " << strerror(-client_fd) << "\n";
            continue;
        }

        std::cout << "Client connected (fd " << client_fd << ")\n";

        // Spawn a new coroutine for this client
        client_handler(ring, client_fd);
    }
}

// ─── Event loop ─────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::ios_base::sync_with_stdio(false);
    std::cout.tie(nullptr);

    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port << " (coroutine)\n";

        Ring ring(4096, IORING_SETUP_SINGLE_ISSUER
                      | IORING_SETUP_DEFER_TASKRUN);

        // Start the accept loop — runs until first co_await
        accept_loop(ring, listen_fd);

        // Event loop: reap CQEs, deliver results, resume coroutines
        for (;;) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                // Recover the IoRequest from user_data
                auto* req = static_cast<IoRequest*>(
                    io_uring_cqe_get_data(cqe));
                int res = cqe->res;
                ring.seen(cqe);

                // Write the result and resume the coroutine
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
