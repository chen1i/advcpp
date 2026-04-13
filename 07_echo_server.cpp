// Exercise 07: io_uring Echo Server
//
// Goal: a TCP echo server that handles multiple concurrent clients
// using io_uring for ALL I/O — accept, recv, send — with zero
// threads and zero epoll.
//
// Why io_uring for networking?
// ────────────────────────────
// Traditional high-perf servers use epoll to learn WHICH fds are
// ready, then issue read()/write() syscalls.  That's two layers:
//
//   epoll_wait → ready fds → read()/write() → back to epoll_wait
//
// With io_uring, you submit the actual I/O operations directly:
//
//   submit [accept, recv, send, ...] → reap completions → submit more
//
// No readiness notifications, no separate syscalls per fd — the
// kernel does the I/O and tells you when it's done.
//
// Key optimisation: BATCHING
// ──────────────────────────
// The event loop drains ALL ready CQEs before calling submit() once.
// This amortises the syscall cost across many operations:
//
//   wait → reap N CQEs, queue M SQEs → one submit() → wait
//
// Without batching, each SQE gets its own submit() syscall and
// io_uring ends up slower than epoll.
//
// New concepts
// ────────────
// - io_uring_prep_accept    — async accept(), returns new client fd
// - io_uring_prep_recv      — async recv() on a connected socket
// - io_uring_prep_send      — async send() echoing data back
// - Encoding operation type + fd in user_data to dispatch CQEs
// - Batch reaping with io_uring_cq_ready + io_uring_peek_cqe
// - Single submit() per event-loop iteration
//
// Usage
// ─────
// Terminal 1:  ./07_echo_server 9000
// Terminal 2:  nc localhost 9000
//              (type messages, see them echoed back)

#include "io_uring_utils.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <iostream>
#include <string_view>
#include <unordered_map>

// ─── Operation types encoded in user_data ───────────────────────────

enum Op : __u64 {
    OP_ACCEPT = 1ULL << 56,
    OP_RECV   = 2ULL << 56,
    OP_SEND   = 3ULL << 56,
};

static constexpr __u64 OP_MASK = 0xFF00'0000'0000'0000ULL;
static constexpr __u64 FD_MASK = 0x00FF'FFFF'FFFF'FFFFULL;

static __u64 encode(Op op, int fd) { return op | static_cast<__u64>(fd); }
static Op    decode_op(__u64 ud)   { return static_cast<Op>(ud & OP_MASK); }
static int   decode_fd(__u64 ud)   { return static_cast<int>(ud & FD_MASK); }

// ─── Per-client state ───────────────────────────────────────────────

static constexpr std::size_t CLIENT_BUF_SIZE = 4096;

struct Client {
    int  fd;
    std::array<char, CLIENT_BUF_SIZE> buf{};
};

// ─── Helpers ────────────────────────────────────────────────────────

static int create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::system_error(errno, std::system_category(), "socket");

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::system_error(errno, std::system_category(), "bind");
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        throw std::system_error(errno, std::system_category(), "listen");
    }

    return fd;
}

int main(int argc, char* argv[])
{
    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port << "\n";

        Ring ring(4096);

        std::unordered_map<int, Client> clients;

        // ── Submit the first accept ─────────────────────────
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));

        // ── Event loop ──────────────────────────────────────
        //
        // Each iteration:
        //   1. submit_and_wait — single syscall that pushes all
        //      pending SQEs AND blocks until ≥1 CQE is ready
        //   2. Drain ALL available CQEs, queuing new SQEs as we go
        //   3. Loop back to 1 (which submits the new SQEs)

        for (;;) {
            unsigned ready = ring.submit_and_wait();

            // Process all available CQEs — no submit() in here,
            // just queue SQEs.  One submit at the end.
            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                __u64 ud = cqe->user_data;
                Op  op  = decode_op(ud);
                int fd  = decode_fd(ud);
                int res = cqe->res;
                ring.seen(cqe);

                switch (op) {

                case OP_ACCEPT: {
                    if (res >= 0) {
                        int client_fd = res;
                        clients[client_fd] = Client{ client_fd };
                        std::cout << "Client connected (fd " << client_fd << ")\n";

                        // Queue recv for the new client
                        auto& c = clients[client_fd];
                        auto* recv_sqe = ring.get_sqe();
                        io_uring_prep_recv(recv_sqe, client_fd,
                                           c.buf.data(), c.buf.size(), 0);
                        io_uring_sqe_set_data64(recv_sqe,
                                                encode(OP_RECV, client_fd));
                    }

                    // Re-arm accept (always, even on error)
                    auto* accept_sqe = ring.get_sqe();
                    io_uring_prep_accept(accept_sqe, listen_fd,
                                         reinterpret_cast<sockaddr*>(&client_addr),
                                         &client_len, 0);
                    io_uring_sqe_set_data64(accept_sqe,
                                            encode(OP_ACCEPT, listen_fd));
                    break;
                }

                case OP_RECV: {
                    if (res <= 0) {
                        if (res < 0)
                            std::cerr << "recv (fd " << fd << "): "
                                      << strerror(-res) << "\n";
                        else
                            std::cout << "Client disconnected (fd " << fd << ")\n";
                        close(fd);
                        clients.erase(fd);
                    } else {
                        // Queue send to echo back
                        auto& c = clients[fd];
                        std::cout << "fd " << fd << ": "
                                  << std::string_view(c.buf.data(), res);
                        auto* send_sqe = ring.get_sqe();
                        io_uring_prep_send(send_sqe, fd,
                                           c.buf.data(), res, 0);
                        io_uring_sqe_set_data64(send_sqe,
                                                encode(OP_SEND, fd));
                    }
                    break;
                }

                case OP_SEND: {
                    if (res < 0) {
                        std::cerr << "send (fd " << fd << "): "
                                  << strerror(-res) << "\n";
                        close(fd);
                        clients.erase(fd);
                    } else {
                        // Re-arm recv
                        auto it = clients.find(fd);
                        if (it != clients.end()) {
                            auto& c = it->second;
                            auto* recv_sqe = ring.get_sqe();
                            io_uring_prep_recv(recv_sqe, fd,
                                               c.buf.data(), c.buf.size(), 0);
                            io_uring_sqe_set_data64(recv_sqe,
                                                    encode(OP_RECV, fd));
                        }
                    }
                    break;
                }

                default:
                    break;
                }
            }

            // No submit here — loop back to submit_and_wait()
            // which pushes all queued SQEs in the same syscall
            // that waits for more CQEs.
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
