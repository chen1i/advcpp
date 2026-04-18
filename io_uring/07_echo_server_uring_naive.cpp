// Exercise 07 (naive): io_uring Echo Server — submit-per-SQE version
//
// Same dispatch logic as 07_echo_server but calls submit() after
// every CQE and waits one at a time.  No batching — for comparison
// with the batched version and with epoll.

#include "07_echo_common.hpp"

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

        // Seed the first accept
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(sqe, encode(OP_ACCEPT, listen_fd));
        ring.submit();

        for (;;) {
            auto* cqe = ring.wait();

            __u64 ud = cqe->user_data;
            int res  = cqe->res;
            ring.seen(cqe);

            handle_cqe(ring, listen_fd, client_addr, client_len,
                       clients, decode_op(ud), decode_fd(ud), res);

            // Submit after every single CQE — no batching
            ring.submit();
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
