#pragma once

// Per-client-buffer echo server logic (exercises 07 and 09).
//
// Provides Client struct and handle_cqe() which queues SQEs
// but never submits — callers control their own submission strategy.

#include "echo_common.hpp"

#include <array>
#include <unordered_map>

// ─── Per-client state ───────────────────────────────────────────────

static constexpr std::size_t CLIENT_BUF_SIZE = 4096;

struct Client {
    int  fd;
    std::array<char, CLIENT_BUF_SIZE> buf{};
};

// ─── CQE dispatch ───────────────────────────────────────────────────

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
