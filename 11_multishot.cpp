// Exercise 11: Multi-shot Accept + Recv
//
// Goal: eliminate the re-arming overhead for accept and recv by
// using multi-shot variants that produce multiple CQEs from a
// single SQE.
//
// Evolution of the echo server:
//
//   07 naive:    submit per SQE                    ~77K msg/s
//   07 batched:  batch submit_and_wait             ~150K msg/s
//   10 bufring:  provided buffers                  ~142K msg/s
//   11 multishot: multi-shot accept + recv          ???
//
// How multi-shot works
// ────────────────────
// Normal (single-shot):
//   submit accept → CQE (new client) → submit accept again → CQE → ...
//
// Multi-shot:
//   submit multishot_accept → CQE → CQE → CQE → ... (forever)
//
// The kernel keeps the SQE alive and produces a new CQE for each
// event.  You can tell a CQE is from a multi-shot SQE by the
// IORING_CQE_F_MORE flag — it means "more CQEs coming from this SQE."
// When the flag is absent, the SQE has been consumed and you need
// to re-submit (e.g. after an error).
//
// Multi-shot recv + provided buffers is the combo: the kernel picks
// a buffer AND delivers data without userspace touching the SQ at all.
//
// New concepts
// ────────────
// - io_uring_prep_multishot_accept — one SQE, many connections
// - IORING_RECV_MULTISHOT flag     — one recv SQE, many data CQEs
// - IORING_CQE_F_MORE             — "this SQE is still alive"
// - Combining multi-shot recv with provided buffer rings

#include "echo_common.hpp"

#include <sys/mman.h>

#include <unordered_set>

static constexpr unsigned BGID     = 0;
static constexpr unsigned NUM_BUFS = 4096;
static constexpr unsigned BUF_SIZE = 4096;

int main(int argc, char* argv[])
{
    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port << " (multishot)\n";

        Ring ring(4096);

        // ── Provided buffer ring (same as exercise 10) ──────

        char* buf_pool = static_cast<char*>(
            mmap(nullptr, NUM_BUFS * BUF_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
        if (buf_pool == MAP_FAILED)
            throw std::system_error(errno, std::system_category(), "mmap");

        int ret;
        auto* br = io_uring_setup_buf_ring(ring.raw(), NUM_BUFS, BGID, 0, &ret);
        if (!br)
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_setup_buf_ring");

        for (unsigned i = 0; i < NUM_BUFS; ++i) {
            io_uring_buf_ring_add(br,
                                  buf_pool + i * BUF_SIZE,
                                  BUF_SIZE, i,
                                  io_uring_buf_ring_mask(NUM_BUFS), i);
        }
        io_uring_buf_ring_advance(br, NUM_BUFS);

        // ── Helpers ─────────────────────────────────────────

        auto recycle_buf = [&](unsigned bid) {
            io_uring_buf_ring_add(br,
                                  buf_pool + bid * BUF_SIZE,
                                  BUF_SIZE, bid,
                                  io_uring_buf_ring_mask(NUM_BUFS), 0);
            io_uring_buf_ring_advance(br, 1);
        };

        // Queue a MULTI-SHOT recv with buffer select
        auto queue_multishot_recv = [&](int fd) {
            auto* sqe = ring.get_sqe();
            io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
            sqe->flags    |= IOSQE_BUFFER_SELECT;
            sqe->ioprio   |= IORING_RECV_MULTISHOT;
            sqe->buf_group  = BGID;
            io_uring_sqe_set_data64(sqe, encode(OP_RECV, fd));
        };

        std::unordered_set<int> clients;

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        // ── Multi-shot accept: ONE SQE, many connections ────
        auto* sqe = ring.get_sqe();
        io_uring_prep_multishot_accept(sqe, listen_fd,
                                       reinterpret_cast<sockaddr*>(&client_addr),
                                       &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));

        // ── Event loop ──────────────────────────────────────

        for (;;) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                __u64 ud    = cqe->user_data;
                Op    op    = decode_op(ud);
                int   fd    = decode_fd(ud);
                int   res   = cqe->res;
                __u32 flags = cqe->flags;
                bool  more  = flags & IORING_CQE_F_MORE;
                ring.seen(cqe);

                switch (op) {

                case OP_ACCEPT: {
                    if (res >= 0) {
                        int client_fd = res;
                        clients.insert(client_fd);
                        std::cout << "Client connected (fd " << client_fd << ")\n";
                        queue_multishot_recv(client_fd);
                    }

                    // If MORE is not set, the multi-shot accept
                    // has been terminated — re-submit it.
                    if (!more) {
                        auto* accept_sqe = ring.get_sqe();
                        io_uring_prep_multishot_accept(accept_sqe, listen_fd,
                            reinterpret_cast<sockaddr*>(&client_addr),
                            &client_len, 0);
                        io_uring_sqe_set_data64(accept_sqe,
                                                encode(OP_ACCEPT, listen_fd));
                    }
                    break;
                }

                case OP_RECV: {
                    if (res == -ENOBUFS) {
                        // Pool exhausted — re-arm after buffers recycle
                        queue_multishot_recv(fd);
                    } else if (res <= 0) {
                        if (res < 0)
                            std::cerr << "recv (fd " << fd << "): "
                                      << strerror(-res) << "\n";
                        else
                            std::cout << "Client disconnected (fd " << fd << ")\n";
                        close(fd);
                        clients.erase(fd);
                    } else {
                        unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
                        char* data = buf_pool + bid * BUF_SIZE;
                        std::cout << "fd " << fd << ": "
                                  << std::string_view(data, res);
                        // Echo back
                        auto* send_sqe = ring.get_sqe();
                        io_uring_prep_send(send_sqe, fd, data, res, 0);
                        io_uring_sqe_set_data64(send_sqe,
                            encode(OP_SEND, fd) | (static_cast<__u64>(bid) << 32));

                        // If MORE is not set, multi-shot recv ended —
                        // re-arm after send completes.
                        // We encode this in a bit so OP_SEND knows.
                        if (!more) {
                            // Use bit 55 to signal "re-arm recv after send"
                            auto ud2 = encode(OP_SEND, fd)
                                     | (static_cast<__u64>(bid) << 32)
                                     | (1ULL << 55);
                            io_uring_sqe_set_data64(send_sqe, ud2);
                        }
                    }
                    break;
                }

                case OP_SEND: {
                    unsigned bid = (ud >> 32) & 0xFFFFFF;
                    bool rearm_recv = (ud >> 55) & 1;
                    recycle_buf(bid);

                    if (res < 0) {
                        std::cerr << "send (fd " << fd << "): "
                                  << strerror(-res) << "\n";
                        close(fd);
                        clients.erase(fd);
                    } else if (rearm_recv && clients.count(fd)) {
                        queue_multishot_recv(fd);
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
