#pragma once

// Shared types for all io_uring echo server variants:
// operation encoding, listener setup.

#include "io_uring_utils.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <iostream>
#include <string_view>
#include <system_error>

// ─── Operation encoding ─────────────────────────────────────────────
//
// Pack the operation type in the upper 8 bits and the fd in the
// lower 56 bits of user_data.

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
