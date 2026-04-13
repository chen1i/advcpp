#pragma once

// Shared types and logic for the io_uring echo server variants.
//
// Both the batched and naive servers use identical CQE handling —
// the only difference is WHEN they call submit().  This header
// provides handle_cqe() which queues SQEs but never submits,
// letting each server control its own submission strategy.

#include "io_uring_utils.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <iostream>
#include <string_view>
#include <unordered_map>

// ─── Operation encoding ─────────────────────────────────────────────

enum Op : __u64 {
    OP_ACCEPT = 1ULL << 56,
    OP_RECV   = 2ULL << 56,
    OP_SEND   = 3ULL << 56,
};

static constexpr __u64 OP_MASK = 0xFF00'0000'0000'0000ULL;
static constexpr __u64 FD_MASK = 0x00FF'FFFF'FFFF'FFFFULL;

inline __u64 encode(Op op, int fd) { return op | static_cast<__u64>(fd); }
inline Op    decode_op(__u64 ud)   { return static_cast<Op>(ud & OP_MASK); }
inline int   decode_fd(__u64 ud)   { return static_cast<int>(ud & FD_MASK); }

// ─── Per-client state ───────────────────────────────────────────────

static constexpr std::size_t CLIENT_BUF_SIZE = 4096;

struct Client {
    int  fd;
    std::array<char, CLIENT_BUF_SIZE> buf{};
};

// ─── Listener setup ─────────────────────────────────────────────────

inline int create_listener(int port)
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

// ─── CQE dispatch ───────────────────────────────────────────────────
//
// Processes one CQE: handles the completed operation and queues the
// follow-up SQE(s).  Does NOT call submit() — the caller decides
// when to flush.

inline void handle_cqe(Ring& ring, int listen_fd,
                        sockaddr_in& client_addr, socklen_t& client_len,
                        std::unordered_map<int, Client>& clients,
                        Op op, int fd, int res)
{
    switch (op) {

    case OP_ACCEPT: {
        if (res >= 0) {
            int client_fd = res;
            clients[client_fd] = Client{ client_fd };
            std::cout << "Client connected (fd " << client_fd << ")\n";

            auto& c = clients[client_fd];
            auto* recv_sqe = ring.get_sqe();
            io_uring_prep_recv(recv_sqe, client_fd,
                               c.buf.data(), c.buf.size(), 0);
            io_uring_sqe_set_data64(recv_sqe, encode(OP_RECV, client_fd));
        }

        // Re-arm accept
        auto* accept_sqe = ring.get_sqe();
        io_uring_prep_accept(accept_sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(accept_sqe, encode(OP_ACCEPT, listen_fd));
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
            auto& c = clients[fd];
            std::cout << "fd " << fd << ": "
                      << std::string_view(c.buf.data(), res);
            auto* send_sqe = ring.get_sqe();
            io_uring_prep_send(send_sqe, fd, c.buf.data(), res, 0);
            io_uring_sqe_set_data64(send_sqe, encode(OP_SEND, fd));
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
            auto it = clients.find(fd);
            if (it != clients.end()) {
                auto& c = it->second;
                auto* recv_sqe = ring.get_sqe();
                io_uring_prep_recv(recv_sqe, fd,
                                   c.buf.data(), c.buf.size(), 0);
                io_uring_sqe_set_data64(recv_sqe, encode(OP_RECV, fd));
            }
        }
        break;
    }

    default:
        break;
    }
}
