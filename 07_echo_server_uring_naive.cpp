// Exercise 07 (naive): io_uring Echo Server — submit-per-SQE version
//
// Same as 07_echo_server but calls submit() after every single SQE
// and waits for one CQE at a time.  For comparison with the batched
// version and with epoll.

#include "io_uring_utils.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <iostream>
#include <string_view>
#include <unordered_map>

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

static constexpr std::size_t CLIENT_BUF_SIZE = 4096;

struct Client {
    int  fd;
    std::array<char, CLIENT_BUF_SIZE> buf{};
};

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
        std::cout << "Listening on port " << port << " (io_uring naive)\n";

        Ring ring(256);

        std::unordered_map<int, Client> clients;

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));
        ring.submit();

        // One CQE at a time, one submit per SQE — no batching

        for (;;) {
            auto* cqe = ring.wait();
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

                    auto& c = clients[client_fd];
                    auto* recv_sqe = ring.get_sqe();
                    io_uring_prep_recv(recv_sqe, client_fd,
                                       c.buf.data(), c.buf.size(), 0);
                    io_uring_sqe_set_data64(recv_sqe,
                                            encode(OP_RECV, client_fd));
                    ring.submit();
                }

                auto* accept_sqe = ring.get_sqe();
                io_uring_prep_accept(accept_sqe, listen_fd,
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_len, 0);
                io_uring_sqe_set_data64(accept_sqe,
                                        encode(OP_ACCEPT, listen_fd));
                ring.submit();
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
                    io_uring_prep_send(send_sqe, fd,
                                       c.buf.data(), res, 0);
                    io_uring_sqe_set_data64(send_sqe,
                                            encode(OP_SEND, fd));
                    ring.submit();
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
                        io_uring_sqe_set_data64(recv_sqe,
                                                encode(OP_RECV, fd));
                        ring.submit();
                    }
                }
                break;
            }

            default:
                break;
            }
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
