// Exercise 16: Boost.Asio Echo Server (io_uring backend)
//
// Same echo server, but using Boost.Asio with C++20 coroutines.
// On Linux with a recent kernel, Asio automatically uses io_uring
// as its backend (since Boost 1.78).
//
// Compare with our hand-rolled coroutine echo server (ex 12):
//
//   Hand-rolled:
//     - IoRequest, Ring, submit_and_wait, manual event loop
//     - ~230 lines
//
//   Asio:
//     - co_spawn, async_read, async_write, use_awaitable
//     - ~80 lines
//     - Cross-platform (epoll/kqueue/IOCP/io_uring)
//
// Asio hides the io_uring details behind its proactor model.
// You write platform-agnostic async code; Asio picks the best
// backend for the OS.
//
// To force io_uring backend, define BOOST_ASIO_HAS_IO_URING
// before including Asio (Asio auto-detects on supported kernels).
//
// Usage
// ─────
// Terminal 1:  ./16_asio_echo 9000
// Terminal 2:  nc localhost 9000

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <iostream>
#include <string_view>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ─── Per-client coroutine ───────────────────────────────────────────
//
// Almost identical to our hand-rolled version, but:
//   - No IoRequest, no Ring, no manual SQE/CQE handling
//   - co_await on async_read_some / async_write replaces our awaitables
//   - Exceptions for errors instead of checking negative results

asio::awaitable<void> client_handler(tcp::socket socket)
{
    int fd = socket.native_handle();
    std::cout << "Client connected (fd " << fd << ")\n";

    try {
        std::array<char, 4096> buf{};

        for (;;) {
            std::size_t n = co_await socket.async_read_some(
                asio::buffer(buf), asio::use_awaitable);

            std::cout << "fd " << fd << ": "
                      << std::string_view(buf.data(), n);

            co_await asio::async_write(
                socket, asio::buffer(buf.data(), n),
                asio::use_awaitable);
        }

    } catch (const boost::system::system_error& e) {
        if (e.code() == asio::error::eof)
            std::cout << "Client disconnected (fd " << fd << ")\n";
        else if (e.code() != asio::error::operation_aborted)
            std::cerr << "fd " << fd << ": " << e.what() << "\n";
    }
}

// ─── Accept loop ────────────────────────────────────────────────────

asio::awaitable<void> accept_loop(tcp::acceptor& acceptor)
{
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(
            asio::use_awaitable);

        // Spawn a new coroutine for this client
        asio::co_spawn(acceptor.get_executor(),
                       client_handler(std::move(socket)),
                       asio::detached);
    }
}

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::ios_base::sync_with_stdio(false);
    std::cout.tie(nullptr);

    int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

    try {
        asio::io_context io(1);  // 1 thread hint

        tcp::acceptor acceptor(io, {tcp::v4(), static_cast<unsigned short>(port)});
        acceptor.set_option(tcp::acceptor::reuse_address(true));

        std::cout << "Listening on port " << port << " (Boost.Asio)\n";

        asio::co_spawn(io, accept_loop(acceptor), asio::detached);

        // Graceful shutdown on SIGINT/SIGTERM
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) {
            std::cout << "\nShutting down...\n";
            acceptor.close();
            io.stop();
        });

        io.run();  // event loop — Asio handles everything

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
