// Exercise 07: io_uring Echo Server — batched version
//
// Drains ALL ready CQEs per iteration, queues follow-up SQEs,
// then does ONE submit_and_wait() to flush them all and block
// for the next batch.  This amortises syscall cost and is the
// key reason io_uring outperforms epoll under load.

#include "07_echo_common.hpp"

int main(int argc, char* argv[])
{
    std::ios_base::sync_with_stdio(false);
    std::cout.tie(nullptr);

    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Listening on port " << port << " (io_uring batched)\n";

        Ring ring(4096, IORING_SETUP_SINGLE_ISSUER
                      | IORING_SETUP_DEFER_TASKRUN);
        std::unordered_map<int, Client> clients;

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        // Seed the first accept
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));

        for (;;) {
            // One syscall: submit all pending SQEs + wait for ≥1 CQE
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                __u64 ud = cqe->user_data;
                int res  = cqe->res;
                ring.seen(cqe);

                handle_cqe(ring, listen_fd, client_addr, client_len,
                           clients, decode_op(ud), decode_fd(ud), res);
            }
            // New SQEs are flushed on the next submit_and_wait()
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
