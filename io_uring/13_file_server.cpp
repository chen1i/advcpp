// Exercise 13: Static File Server — io_uring for Disk + Network
//
// Goal: serve files over HTTP using a single io_uring ring for
// both network I/O (accept, recv, send) and file I/O (read).
// Everything is async, driven by coroutines from exercise 12.
//
// This is the capstone: it combines nearly every concept so far:
//   - Ring, FileDescriptor RAII wrappers     (ex 01)
//   - Batched submit_and_wait                (ex 07)
//   - SINGLE_ISSUER + DEFER_TASKRUN          (ex 11)
//   - Coroutine per connection               (ex 12)
//   - Unified disk + network I/O in one ring (new)
//
// Usage
// ─────
// Terminal 1:  ./13_file_server 8080
// Terminal 2:  curl http://localhost:8080/sample.txt
//
// The server serves files relative to the current working directory.
// It handles only GET requests and returns 404 for missing files.

#include "echo_common.hpp"

#include <sys/stat.h>

#include <array>
#include <coroutine>
#include <sstream>
#include <string>

// ─── Task (same as ex 12) ───────────────────────────────────────────

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never  initial_suspend() { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// ─── IoRequest + Awaitables ─────────────────────────────────────────

struct IoRequest {
    std::coroutine_handle<> handle;
    int result = 0;
};

struct AsyncAccept : IoRequest {
    Ring& ring;
    int   listen_fd;
    sockaddr_in  addr{};
    socklen_t    len = sizeof(addr);

    AsyncAccept(Ring& r, int lfd) : ring(r), listen_fd(lfd) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(&addr), &len, 0);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

struct AsyncRecv : IoRequest {
    Ring& ring;
    int fd; char* buf; unsigned len;

    AsyncRecv(Ring& r, int f, char* b, unsigned l)
        : ring(r), fd(f), buf(b), len(l) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

struct AsyncSend : IoRequest {
    Ring& ring;
    int fd; const char* buf; unsigned len;

    AsyncSend(Ring& r, int f, const char* b, unsigned l)
        : ring(r), fd(f), buf(b), len(l) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_send(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

struct AsyncRead : IoRequest {
    Ring& ring;
    int fd; char* buf; unsigned len; off_t offset;

    AsyncRead(Ring& r, int f, char* b, unsigned l, off_t o)
        : ring(r), fd(f), buf(b), len(l), offset(o) {}
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        auto* sqe = ring.get_sqe();
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, static_cast<IoRequest*>(this));
    }
    int await_resume() { return result; }
};

// ─── Send all helper ────────────────────────────────────────────────

Task async_send_all(Ring& ring, int fd, const char* data, int len)
{
    while (len > 0) {
        AsyncSend op(ring, fd, data, len);
        int n = co_await op;
        if (n <= 0) break;
        data += n;
        len  -= n;
    }
}

// ─── HTTP helpers ───────────────────────────────────────────────────

static std::string parse_path(const char* request, int len)
{
    // "GET /path HTTP/1.x\r\n..."
    std::string_view req(request, len);
    if (req.substr(0, 4) != "GET ")
        return "";

    auto end = req.find(' ', 4);
    if (end == std::string_view::npos)
        return "";

    std::string path(req.substr(4, end - 4));

    // Strip leading /
    if (!path.empty() && path[0] == '/')
        path = path.substr(1);

    // Default to index
    if (path.empty())
        path = "sample.txt";

    // Block path traversal
    if (path.find("..") != std::string::npos)
        return "";

    return path;
}

static std::string make_response_header(int status, const char* status_text,
                                         off_t content_length,
                                         const char* content_type = "text/plain")
{
    std::ostringstream oss;
    oss << "HTTP/1.0 " << status << " " << status_text << "\r\n"
        << "Content-Length: " << content_length << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    return oss.str();
}

// ─── Per-client coroutine ───────────────────────────────────────────

Task client_handler(Ring& ring, int fd)
{
    std::array<char, 4096> buf{};

    // 1. Receive the HTTP request
    AsyncRecv recv_op(ring, fd, buf.data(), buf.size() - 1);
    int n = co_await recv_op;

    if (n <= 0) {
        close(fd);
        co_return;
    }
    buf[n] = '\0';

    // 2. Parse the requested path
    std::string path = parse_path(buf.data(), n);
    std::cout << "fd " << fd << ": GET /" << path << "\n";

    if (path.empty()) {
        auto resp = make_response_header(400, "Bad Request", 0);
        AsyncSend send_op(ring, fd, resp.data(), resp.size());
        co_await send_op;
        close(fd);
        co_return;
    }

    // 3. Open the file
    int file_fd = open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        std::string body = "404 Not Found: " + path + "\n";
        auto resp = make_response_header(404, "Not Found", body.size());
        resp += body;
        AsyncSend send_op(ring, fd, resp.data(), resp.size());
        co_await send_op;
        close(fd);
        co_return;
    }

    // 4. Get file size
    struct stat st{};
    fstat(file_fd, &st);
    off_t file_size = st.st_size;

    // 5. Send HTTP header
    auto header = make_response_header(200, "OK", file_size);
    {
        AsyncSend send_op(ring, fd, header.data(), header.size());
        int sent = co_await send_op;
        if (sent <= 0) {
            close(file_fd);
            close(fd);
            co_return;
        }
    }

    // 6. Read file and send in chunks — both via io_uring
    off_t offset = 0;
    while (offset < file_size) {
        unsigned chunk = std::min<off_t>(file_size - offset, buf.size());

        AsyncRead read_op(ring, file_fd, buf.data(), chunk, offset);
        int bytes_read = co_await read_op;
        if (bytes_read <= 0) break;

        AsyncSend send_op(ring, fd, buf.data(), bytes_read);
        int sent = co_await send_op;
        if (sent <= 0) break;

        offset += bytes_read;
    }

    close(file_fd);
    close(fd);
}

// ─── Accept loop ────────────────────────────────────────────────────

Task accept_loop(Ring& ring, int listen_fd)
{
    for (;;) {
        AsyncAccept accept_op(ring, listen_fd);
        int client_fd = co_await accept_op;

        if (client_fd < 0) {
            std::cerr << "accept: " << strerror(-client_fd) << "\n";
            continue;
        }

        std::cout << "Client connected (fd " << client_fd << ")\n";
        client_handler(ring, client_fd);
    }
}

// ─── Event loop ─────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::ios_base::sync_with_stdio(false);
    std::cout.tie(nullptr);

    int port = (argc > 1) ? std::stoi(argv[1]) : 8080;

    try {
        int listen_fd = create_listener(port);
        std::cout << "Serving files from current directory on port "
                  << port << "\n";

        Ring ring(4096, IORING_SETUP_SINGLE_ISSUER
                      | IORING_SETUP_DEFER_TASKRUN);

        accept_loop(ring, listen_fd);

        for (;;) {
            unsigned ready = ring.submit_and_wait();

            for (unsigned j = 0; j < ready; ++j) {
                auto* cqe = ring.peek();
                if (!cqe) break;

                auto* req = static_cast<IoRequest*>(
                    io_uring_cqe_get_data(cqe));
                int res = cqe->res;
                ring.seen(cqe);

                req->result = res;
                req->handle.resume();
            }

            std::cout.flush();
        }

        close(listen_fd);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
