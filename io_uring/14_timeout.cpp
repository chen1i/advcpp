// Exercise 14: Timeouts and Cancellation
//
// Goal: add idle connection timeouts to the echo server.
// If a client doesn't send data within 5 seconds, disconnect it.
//
// How io_uring timeouts work
// ──────────────────────────
// Option 1: LINKED TIMEOUT (used here)
//   Link a timeout SQE after a recv SQE.  If recv doesn't complete
//   within the timeout, BOTH are cancelled:
//
//     recv ──LINK──> link_timeout(5s)
//
//   - recv completes in time  → recv CQE (success), timeout CQE (-ECANCELED)
//   - timeout fires first     → recv CQE (-ECANCELED), timeout CQE (-ETIME)
//
//   Two CQEs always arrive — one for each SQE in the chain.
//
// Option 2: STANDALONE TIMEOUT + CANCEL
//   Submit recv and a separate timeout.  When the timeout fires,
//   manually cancel the recv with io_uring_prep_cancel.
//   More flexible but more complex.
//
// New concepts
// ────────────
// - io_uring_prep_link_timeout — timeout linked to previous SQE
// - __kernel_timespec          — kernel's timespec for io_uring
// - IOSQE_IO_LINK              — chain the timeout to the recv
// - -ECANCELED / -ETIME        — CQE results for cancelled/timed-out ops
// - Handling two CQEs per recv (recv + its linked timeout)

#include "echo_common.hpp"

#include <array>
#include <coroutine>

static constexpr int IDLE_TIMEOUT_SEC = 5;

// ─── Task (same as ex 12) ───────────────────────────────────────────

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
    Ring& ring;
    int listen_fd;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);

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

struct AsyncSend : IoRequest {
    Ring& ring;
    int fd; const char* buf; unsigned len;

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

// ─── Recv with linked timeout ───────────────────────────────────────
//
// Submits TWO SQEs: recv ──LINK──> link_timeout.
// The coroutine is resumed after BOTH CQEs arrive.
// Returns the recv result: positive = data, -ECANCELED = timed out.

struct AsyncRecvTimeout : IoRequest {
    Ring& ring;
    int fd; char* buf; unsigned len;
    int timeout_sec;

    // Dummy IoRequest for the timeout CQE — the event loop
    // resumes noop_coroutine which does nothing.
    IoRequest timeout_req;

    AsyncRecvTimeout(Ring& r, int f, char* b, unsigned l, int t)
        : ring(r), fd(f), buf(b), len(l), timeout_sec(t) {}

    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;

        // SQE 1: recv with LINK flag
        auto* recv_sqe = ring.get_sqe();
        io_uring_prep_recv(recv_sqe, fd, buf, len, 0);
        recv_sqe->flags |= IOSQE_IO_LINK;
        io_uring_sqe_set_data(recv_sqe, static_cast<IoRequest*>(this));

        // SQE 2: linked timeout
        auto* timeout_sqe = ring.get_sqe();
        ts_ = { .tv_sec = timeout_sec, .tv_nsec = 0 };
        io_uring_prep_link_timeout(timeout_sqe, &ts_, 0);
        // The timeout CQE needs its own IoRequest so the event
        // loop can handle it.  We use noop_coroutine — resuming
        // it does nothing.  Only the recv CQE resumes our real
        // coroutine.
        timeout_req.handle = std::noop_coroutine();
        io_uring_sqe_set_data(timeout_sqe,
                              static_cast<IoRequest*>(&timeout_req));
    }

    int await_resume() { return result; }

private:
    __kernel_timespec ts_{};
};

// ─── Per-client coroutine ───────────────────────────────────────────

Task client_handler(Ring& ring, int fd)
{
    std::array<char, 4096> buf{};

    for (;;) {
        AsyncRecvTimeout recv_op(ring, fd, buf.data(), buf.size(),
                                 IDLE_TIMEOUT_SEC);
        int n = co_await recv_op;

        if (n == -ECANCELED) {
            std::cout << "fd " << fd << ": idle timeout ("
                      << IDLE_TIMEOUT_SEC << "s)\n";
            break;
        }

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

// ─── Accept loop ────────────────────────────────────────────────────

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
        std::cout << "Listening on port " << port
                  << " (coroutine + " << IDLE_TIMEOUT_SEC << "s timeout)\n";

        Ring ring(4096, IORING_SETUP_SINGLE_ISSUER
                      | IORING_SETUP_DEFER_TASKRUN);

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
