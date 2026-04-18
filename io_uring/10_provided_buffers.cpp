// Exercise 10: Provided Buffer Rings
//
// Goal: reduce per-connection memory from O(clients) to O(active)
// by letting the kernel pick buffers from a shared pool.
//
// The problem
// ───────────
// In exercise 07, every client has its own 4K buffer:
//
//   10,000 connections × 4K = 40 MB    (most of it idle)
//
// With provided buffers, the kernel picks a free buffer from a
// shared pool ONLY when data arrives.  If you have 10K connections
// but only 100 are active at any moment, you need ~100 buffers.
//
// How it works
// ────────────
// 1. Allocate a ring of buffers and register them with the kernel
//    using io_uring_register_buf_ring()
// 2. Submit recv with IOSQE_BUFFER_SELECT — no buffer pointer!
//    The kernel will pick one when data is ready
// 3. The CQE tells you WHICH buffer was used (via IORING_CQE_F_BUFFER
//    flag and buffer ID in cqe->flags)
// 4. After processing, return the buffer to the pool
//
// New concepts
// ────────────
// - io_uring_buf_ring_init / io_uring_register_buf_ring
// - io_uring_buf_ring_add  — return a buffer to the pool
// - IOSQE_BUFFER_SELECT    — tell the kernel to pick a buffer
// - IORING_CQE_F_BUFFER    — CQE flag indicating which buffer was used
// - Buffer group ID (bgid)  — identifies the buffer pool

#include "echo_common.hpp"

#include <sys/mman.h>

#include <unordered_set>

// ─── Buffer pool constants ──────────────────────────────────────────

static constexpr unsigned BGID       = 0;      // buffer group ID
static constexpr unsigned NUM_BUFS   = 4096;   // pool size — must cover peak
                                                // concurrent recv+send in flight
static constexpr unsigned BUF_SIZE   = 4096;   // bytes per buffer
int main(int argc, char* argv[])
{
    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port
                  << " (provided buffers, pool=" << NUM_BUFS << ")\n";

        Ring ring(4096);

        // ── Set up the provided buffer ring ─────────────────
        //
        // io_uring_setup_buf_ring() allocates the ring structure,
        // registers it with the kernel, and returns a pointer — all
        // in one call.  We then add our data buffers to the ring.

        // Allocate the data buffers (page-aligned)
        char* buf_pool = static_cast<char*>(
            mmap(nullptr, NUM_BUFS * BUF_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
        if (buf_pool == MAP_FAILED)
            throw std::system_error(errno, std::system_category(), "mmap");

        // Allocate and register the buffer ring
        int ret;
        auto* br = io_uring_setup_buf_ring(ring.raw(), NUM_BUFS, BGID, 0, &ret);
        if (!br)
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_setup_buf_ring");

        // Add all buffers to the ring
        for (unsigned i = 0; i < NUM_BUFS; ++i) {
            io_uring_buf_ring_add(br,
                                  buf_pool + i * BUF_SIZE,
                                  BUF_SIZE,
                                  /*bid=*/i,
                                  io_uring_buf_ring_mask(NUM_BUFS),
                                  i);
        }
        io_uring_buf_ring_advance(br, NUM_BUFS);

        // ── Helpers ─────────────────────────────────────────

        // Return a buffer to the pool after use
        auto recycle_buf = [&](unsigned bid) {
            io_uring_buf_ring_add(br,
                                  buf_pool + bid * BUF_SIZE,
                                  BUF_SIZE,
                                  bid,
                                  io_uring_buf_ring_mask(NUM_BUFS),
                                  0);
            io_uring_buf_ring_advance(br, 1);
        };

        // Queue a recv with BUFFER_SELECT (no buffer pointer)
        auto queue_recv = [&](int fd) {
            auto* sqe = ring.get_sqe();
            io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
            sqe->flags |= IOSQE_BUFFER_SELECT;
            sqe->buf_group = BGID;
            io_uring_sqe_set_data64(sqe, encode(OP_RECV, fd));
        };

        // ── State: just track which fds are connected ───────
        // No per-client buffer needed!
        std::unordered_set<int> clients;

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        // Seed the first accept
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));

        // ── Event loop ──────────────────────────────────────

        for (;;) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                __u64 ud  = cqe->user_data;
                Op    op  = decode_op(ud);
                int   fd  = decode_fd(ud);
                int   res = cqe->res;
                __u32 flags = cqe->flags;
                ring.seen(cqe);

                switch (op) {

                case OP_ACCEPT: {
                    if (res >= 0) {
                        int client_fd = res;
                        clients.insert(client_fd);
                        std::cout << "Client connected (fd " << client_fd << ")\n";
                        queue_recv(client_fd);
                    }

                    // Re-arm accept
                    auto* accept_sqe = ring.get_sqe();
                    io_uring_prep_accept(accept_sqe, listen_fd,
                                         reinterpret_cast<sockaddr*>(&client_addr),
                                         &client_len, 0);
                    io_uring_sqe_set_data64(accept_sqe,
                                            encode(OP_ACCEPT, listen_fd));
                    break;
                }

                case OP_RECV: {
                    if (res == -ENOBUFS) {
                        // Buffer pool exhausted — re-queue and try later
                        // once in-flight sends recycle their buffers.
                        queue_recv(fd);
                    } else if (res <= 0) {
                        if (res < 0)
                            std::cerr << "recv (fd " << fd << "): "
                                      << strerror(-res) << "\n";
                        else
                            std::cout << "Client disconnected (fd " << fd << ")\n";
                        close(fd);
                        clients.erase(fd);
                    } else {
                        // The kernel picked a buffer for us
                        unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;

                        char* data = buf_pool + bid * BUF_SIZE;
                        std::cout << "fd " << fd << ": "
                                  << std::string_view(data, res);

                        // Echo back from the same pooled buffer
                        auto* send_sqe = ring.get_sqe();
                        io_uring_prep_send(send_sqe, fd, data, res, 0);
                        // Encode both fd and bid so we can recycle after send
                        io_uring_sqe_set_data64(send_sqe,
                            encode(OP_SEND, fd) | (static_cast<__u64>(bid) << 32));
                    }
                    break;
                }

                case OP_SEND: {
                    // Extract bid from user_data (bits 32-55)
                    unsigned bid = (ud >> 32) & 0xFFFFFF;
                    recycle_buf(bid);

                    if (res < 0) {
                        std::cerr << "send (fd " << fd << "): "
                                  << strerror(-res) << "\n";
                        close(fd);
                        clients.erase(fd);
                    } else {
                        if (clients.count(fd))
                            queue_recv(fd);
                    }
                    break;
                }

                default:
                    break;
                }
            }
        }

        munmap(buf_pool, NUM_BUFS * BUF_SIZE);
        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
