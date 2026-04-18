// Echo server benchmark client using io_uring for networking.
//
// Spawns N connections, each sends R round-trip messages, all
// driven by a single io_uring ring — same completion-based model
// as the server.
//
// Usage:
//   ./08_echo_bench -n 1000 -r 1000 -p 9000

#include "io_uring_utils.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// ─── Operation encoding (same scheme as the server) ─────────────────

enum Op : __u64 {
    OP_CONNECT = 1ULL << 56,
    OP_SEND    = 2ULL << 56,
    OP_RECV    = 3ULL << 56,
};

static constexpr __u64 OP_MASK = 0xFF00'0000'0000'0000ULL;
static constexpr __u64 ID_MASK = 0x00FF'FFFF'FFFF'FFFFULL;

static __u64 encode(Op op, int id) { return op | static_cast<__u64>(id); }
static Op    decode_op(__u64 ud)   { return static_cast<Op>(ud & OP_MASK); }
static int   decode_id(__u64 ud)   { return static_cast<int>(ud & ID_MASK); }

// ─── Per-client state ───────────────────────────────────────────────

static constexpr std::size_t MSG_SIZE = 32;

struct Client {
    int  fd        = -1;
    int  remaining = 0;    // rounds left
    bool ok        = true; // all echoes matched?
    char send_buf[MSG_SIZE];
    char recv_buf[MSG_SIZE];
};

int main(int argc, char* argv[])
{
    int num_clients = 100;
    int rounds      = 100;
    int port        = 9000;
    const char* host = "127.0.0.1";

    // ── Minimal arg parsing ─────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) num_clients = std::stoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rounds = std::stoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
    }

    long long total_msgs = static_cast<long long>(num_clients) * rounds;
    std::cout << "Spawning " << num_clients << " clients, "
              << rounds << " rounds each -> "
              << total_msgs << " total messages\n";

    try {
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port   = htons(port);
        inet_pton(AF_INET, host, &server_addr.sin_addr);

        // Allocate clients
        std::vector<Client> clients(num_clients);
        for (int i = 0; i < num_clients; ++i) {
            clients[i].remaining = rounds;
            snprintf(clients[i].send_buf, MSG_SIZE,
                     "client %d ping\n", i);
        }

        // Ring big enough for all concurrent operations
        Ring ring(std::max(4096u, static_cast<unsigned>(num_clients * 2)));

        auto t0 = std::chrono::steady_clock::now();

        // ── Create sockets and submit connects ──────────────
        for (int i = 0; i < num_clients; ++i) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                throw std::system_error(errno, std::system_category(), "socket");
            clients[i].fd = fd;

            auto* sqe = ring.get_sqe();
            io_uring_prep_connect(sqe, fd,
                reinterpret_cast<const sockaddr*>(&server_addr),
                sizeof(server_addr));
            io_uring_sqe_set_data64(sqe, encode(OP_CONNECT, i));
        }

        // ── Event loop ──────────────────────────────────────
        int clients_done = 0;

        while (clients_done < num_clients) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                __u64 ud = cqe->user_data;
                Op  op = decode_op(ud);
                int id = decode_id(ud);
                int res = cqe->res;
                ring.seen(cqe);

                auto& c = clients[id];

                switch (op) {

                case OP_CONNECT: {
                    if (res < 0) {
                        std::cerr << "connect client " << id << ": "
                                  << strerror(-res) << "\n";
                        c.ok = false;
                        ++clients_done;
                    } else {
                        // Connected — send first message
                        int len = strlen(c.send_buf);
                        auto* sqe = ring.get_sqe();
                        io_uring_prep_send(sqe, c.fd,
                                           c.send_buf, len, 0);
                        io_uring_sqe_set_data64(sqe, encode(OP_SEND, id));
                    }
                    break;
                }

                case OP_SEND: {
                    if (res < 0) {
                        c.ok = false;
                        ++clients_done;
                    } else {
                        // Send done — recv the echo
                        auto* sqe = ring.get_sqe();
                        io_uring_prep_recv(sqe, c.fd,
                                           c.recv_buf, MSG_SIZE, 0);
                        io_uring_sqe_set_data64(sqe, encode(OP_RECV, id));
                    }
                    break;
                }

                case OP_RECV: {
                    if (res <= 0) {
                        c.ok = false;
                        ++clients_done;
                    } else {
                        // Verify echo
                        int len = strlen(c.send_buf);
                        if (res != len ||
                            memcmp(c.recv_buf, c.send_buf, len) != 0) {
                            c.ok = false;
                        }

                        --c.remaining;
                        if (c.remaining <= 0) {
                            close(c.fd);
                            c.fd = -1;
                            ++clients_done;
                        } else {
                            // Next round — send again
                            auto* sqe = ring.get_sqe();
                            io_uring_prep_send(sqe, c.fd,
                                               c.send_buf, len, 0);
                            io_uring_sqe_set_data64(sqe,
                                                    encode(OP_SEND, id));
                        }
                    }
                    break;
                }

                default:
                    break;
                }
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();

        // ── Report ──────────────────────────────────────────
        int passed = 0, failed = 0;
        for (auto& c : clients) {
            if (c.ok) ++passed; else ++failed;
            if (c.fd >= 0) close(c.fd);
        }

        std::cout << "\n" << passed << "/" << num_clients
                  << " clients passed, "
                  << total_msgs << " messages in "
                  << std::fixed << std::setprecision(3) << elapsed << "s ("
                  << static_cast<long long>(total_msgs / elapsed)
                  << " msg/s)\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
