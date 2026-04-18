// Exercise 15: Graceful Shutdown with Cancellation
//
// Goal: add clean Ctrl-C handling to the coroutine echo server.
// On SIGINT/SIGTERM:
//   1. Stop accepting new connections
//   2. Cancel all in-flight recv/send operations
//   3. Close all client sockets
//   4. Exit cleanly
//
// How signals work with io_uring
// ──────────────────────────────
// Traditional signal handlers are problematic with io_uring —
// they interrupt syscalls unpredictably.  Instead, we:
//
//   1. Block SIGINT/SIGTERM with sigprocmask
//   2. Create a signalfd that becomes readable when the signal arrives
//   3. Submit an io_uring poll on the signalfd
//   4. When the CQE fires, we know a signal arrived — initiate shutdown
//
// Everything stays in the io_uring event loop.  No signal handler,
// no race conditions, no async-signal-safety worries.
//
// New concepts
// ────────────
// - signalfd                    — convert signals to fd events
// - io_uring_prep_poll_add      — poll an fd for readiness (like epoll)
// - io_uring_prep_cancel64      — cancel SQEs by user_data value
// - IORING_ASYNC_CANCEL_ALL     — cancel all matching operations
// - Clean resource tracking for shutdown

#include "echo_common.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>

#include <array>
#include <coroutine>
#include <unordered_set>

// ─── Task ───────────────────────────────────────────────────────────

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never  initial_suspend() { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// ─── IoRequest base ─────────────────────────────────────────────────

struct IoRequest {
    std::coroutine_handle<> handle;
    int result = 0;
};

// ─── Awaitables ─────────────────────────────────────────────────────

struct AsyncAccept : IoRequest {
    Ring& ring; int listen_fd;
    sockaddr_in addr{}; socklen_t len = sizeof(addr);

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
    Ring& ring; int fd; char* buf; unsigned len;

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
    Ring& ring; int fd; const char* buf; unsigned len;

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

struct AsyncPoll : IoRequest {
    Ring& ring; int fd; unsigned mask;

    AsyncPoll(Ring& r, int f, unsigned m)
        : ring(r), fd(f), mask(m) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_poll_add(sqe, fd, mask);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

// ─── Shared state ───────────────────────────────────────────────────

struct ServerState {
    std::unordered_set<int> client_fds;
    bool shutting_down = false;
};

// ─── Per-client coroutine ───────────────────────────────────────────

Task client_handler(Ring& ring, int fd, ServerState& state)
{
    state.client_fds.insert(fd);
    std::array<char, 4096> buf{};

    for (;;) {
        AsyncRecv recv_op(ring, fd, buf.data(), buf.size());
        int n = co_await recv_op;

        if (n <= 0) {
            if (n == -ECANCELED)
                std::cout << "fd " << fd << ": cancelled (shutdown)\n";
            else if (n < 0)
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
        if (sent <= 0) break;
    }

    close(fd);
    state.client_fds.erase(fd);
}

// ─── Accept loop ────────────────────────────────────────────────────

Task accept_loop(Ring& ring, int listen_fd, ServerState& state)
{
    for (;;) {
        AsyncAccept accept_op(ring, listen_fd);
        int client_fd = co_await accept_op;

        if (client_fd < 0) {
            if (client_fd == -ECANCELED) {
                std::cout << "Accept cancelled (shutdown)\n";
                break;
            }
            std::cerr << "accept: " << strerror(-client_fd) << "\n";
            continue;
        }

        std::cout << "Client connected (fd " << client_fd << ")\n";
        client_handler(ring, client_fd, state);
    }
}

// ─── Signal watcher coroutine ───────────────────────────────────────

Task signal_watcher(Ring& ring, int signal_fd, int listen_fd,
                    ServerState& state)
{
    // Wait for the signalfd to become readable (signal arrived)
    AsyncPoll poll_op(ring, signal_fd, POLLIN);
    co_await poll_op;

    // Read the signal info (clears the signalfd)
    signalfd_siginfo info{};
    read(signal_fd, &info, sizeof(info));
    std::cout << "\nReceived signal " << info.ssi_signo
              << ", shutting down...\n";

    state.shutting_down = true;

    // Cancel the accept by cancelling all SQEs on the listen fd
    auto* sqe = ring.get_sqe();
    io_uring_prep_cancel_fd(sqe, listen_fd, IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data(sqe, nullptr);

    // Cancel all in-flight client operations
    for (int fd : state.client_fds) {
        auto* cancel_sqe = ring.get_sqe();
        io_uring_prep_cancel_fd(cancel_sqe, fd, IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data(cancel_sqe, nullptr);
    }
}

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::ios_base::sync_with_stdio(false);
    std::cout.tie(nullptr);

    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        // ── Block SIGINT/SIGTERM, create signalfd ───────────
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, nullptr);

        int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);
        if (signal_fd < 0)
            throw std::system_error(errno, std::system_category(), "signalfd");

        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port
                  << " (Ctrl-C for graceful shutdown)\n";

        Ring ring(4096, IORING_SETUP_SINGLE_ISSUER
                      | IORING_SETUP_DEFER_TASKRUN);

        ServerState state;

        // Start coroutines
        accept_loop(ring, listen_fd, state);
        signal_watcher(ring, signal_fd, listen_fd, state);

        // Event loop — exits when shutting_down and no clients left
        for (;;) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                auto* req = static_cast<IoRequest*>(
                    io_uring_cqe_get_data(cqe));
                int res = cqe->res;
                ring.seen(cqe);

                // Cancel CQEs have nullptr user_data — skip
                if (!req) continue;

                req->result = res;
                req->handle.resume();
            }

            std::cout.flush();

            if (state.shutting_down && state.client_fds.empty())
                break;
        }

        std::cout << "All clients disconnected, exiting.\n";

        close(signal_fd);
        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
