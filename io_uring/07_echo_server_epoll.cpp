// Exercise 07b: epoll Echo Server (for comparison with 07_echo_server)
//
// Same behaviour — single-threaded TCP echo — but using the
// traditional epoll readiness-notification model:
//
//   epoll_wait → which fds are ready → read()/write()/accept() → loop
//
// Contrast with io_uring's completion model:
//
//   submit [accept, recv, send] → reap completions → submit more
//
// The key architectural difference:
//   epoll tells you a fd is READY, then you do the syscall yourself.
//   io_uring does the I/O for you and tells you when it's DONE.

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string_view>
#include <unordered_map>

static constexpr int MAX_EVENTS = 256;
static constexpr std::size_t BUF_SIZE = 4096;

struct Client {
    int fd;
    std::array<char, BUF_SIZE> buf{};
};

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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

    set_nonblocking(fd);
    return fd;
}

int main(int argc, char* argv[])
{
    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port << " (epoll)\n";

        int epfd = epoll_create1(0);
        if (epfd < 0)
            throw std::system_error(errno, std::system_category(), "epoll_create1");

        // Watch the listener for incoming connections
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        std::unordered_map<int, Client> clients;
        std::array<epoll_event, MAX_EVENTS> events{};

        for (;;) {
            int n = epoll_wait(epfd, events.data(), events.size(), -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::system_category(), "epoll_wait");
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;

                if (fd == listen_fd) {
                    // ── Accept all pending connections ───────
                    for (;;) {
                        sockaddr_in client_addr{};
                        socklen_t   client_len = sizeof(client_addr);
                        int client_fd = accept(listen_fd,
                            reinterpret_cast<sockaddr*>(&client_addr),
                            &client_len);

                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;  // no more pending
                            std::cerr << "accept: " << strerror(errno) << "\n";
                            break;
                        }

                        set_nonblocking(client_fd);
                        clients[client_fd] = Client{ client_fd };
                        std::cout << "Client connected (fd " << client_fd << ")\n";

                        epoll_event cev{};
                        cev.events  = EPOLLIN;
                        cev.data.fd = client_fd;
                        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
                    }

                } else {
                    // ── Read from client and echo back ──────
                    auto it = clients.find(fd);
                    if (it == clients.end()) continue;
                    auto& c = it->second;

                    ssize_t nr = read(fd, c.buf.data(), c.buf.size());

                    if (nr <= 0) {
                        if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                            continue;
                        if (nr < 0)
                            std::cerr << "recv (fd " << fd << "): "
                                      << strerror(errno) << "\n";
                        else
                            std::cout << "Client disconnected (fd " << fd << ")\n";
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        clients.erase(it);
                        continue;
                    }

                    std::cout << "fd " << fd << ": "
                              << std::string_view(c.buf.data(), nr);

                    // Write back — for simplicity, assume full write.
                    // A production server would handle partial writes
                    // with EPOLLOUT.
                    ssize_t nw = write(fd, c.buf.data(), nr);
                    if (nw < 0) {
                        std::cerr << "send (fd " << fd << "): "
                                  << strerror(errno) << "\n";
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        clients.erase(it);
                    }
                }
            }
        }

        close(epfd);
        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
