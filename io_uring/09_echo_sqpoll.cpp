// Exercise 09: SQPOLL — Zero-syscall Echo Server
//
// Goal: eliminate the submit() syscall entirely by having a
// dedicated kernel thread poll the submission queue.
//
// Evolution of the submission strategy:
//
//   naive:    submit() after every SQE          ~77K msg/s
//   batched:  one submit_and_wait() per batch   ~150K msg/s
//   SQPOLL:   no submit() at all                ???
//
// How SQPOLL works
// ────────────────
// With IORING_SETUP_SQPOLL, the kernel spawns a polling thread
// (sq_thread) that continuously checks the submission queue for
// new entries.  As soon as userspace writes an SQE and advances
// the SQ tail, the kernel thread picks it up — no syscall needed.
//
// The polling thread goes to sleep after sq_thread_idle ms of
// inactivity.  When it sleeps, the IORING_SQ_NEED_WAKEUP flag
// is set on the SQ ring, and the next io_uring_submit() or
// io_uring_enter() wakes it back up.
//
// Trade-offs
// ──────────
// + Zero submit() syscalls in the hot path
// + Lowest possible submission latency
// - Burns one CPU core for the polling thread
// - Requires CAP_SYS_NICE (or root) on kernels < 5.12
// - Only worth it for high-throughput, latency-sensitive workloads
//
// New concepts
// ────────────
// - IORING_SETUP_SQPOLL flag
// - sq_thread_idle — how long the poller spins before sleeping
// - IORING_SQ_NEED_WAKEUP — check if the poller needs a kick
// - io_uring_sqring_wait() — block until the kernel consumes SQEs
//   (replaces submit_and_wait for flow control)

#include "07_echo_common.hpp"

int main(int argc, char* argv[])
{
    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port << " (io_uring SQPOLL)\n";

        // ── Init ring with SQPOLL ───────────────────────────
        //
        // The kernel thread will poll the SQ for up to 1000ms
        // before going to sleep.

        struct io_uring_params params{};
        params.flags        = IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 1000;  // ms

        Ring ring(4096, params.flags);

        // Register the listen fd so the SQPOLL thread can access it.
        // SQPOLL requires fds to be registered or uses direct descriptors.
        // For simplicity we register the listen socket and use plain fds
        // for client sockets (the kernel allows both on modern kernels).

        std::unordered_map<int, Client> clients;

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        // Seed the first accept
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));

        // Kick the SQ poller for the initial accept.
        // After this, the poller picks up new SQEs automatically.
        ring.submit();

        // ── Event loop ──────────────────────────────────────
        //
        // We only need to wait for CQEs — submission happens
        // automatically via the kernel polling thread.
        // We still call submit() as a no-op/wakeup if the
        // poller has gone to sleep (handled inside liburing).

        for (;;) {
            unsigned ready = ring.wait_batch();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                __u64 ud = cqe->user_data;
                int res  = cqe->res;
                ring.seen(cqe);

                handle_cqe(ring, listen_fd, client_addr, client_len,
                           clients, decode_op(ud), decode_fd(ud), res);
            }

            // With SQPOLL, the kernel thread picks up new SQEs
            // from the SQ ring automatically.  We only need
            // submit() to wake the thread if it went to sleep.
            // io_uring_submit() in liburing checks NEED_WAKEUP
            // and skips the syscall if the poller is already running.
            ring.submit();
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
