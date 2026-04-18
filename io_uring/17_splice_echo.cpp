// Exercise 17: Zero-copy Echo with splice
//
// Goal: echo data back to the client WITHOUT copying it through
// userspace.  The data path:
//
//   Normal echo (ex 12):
//     client socket ──recv──> userspace buf ──send──> client socket
//     (2 copies: kernel→user, user→kernel)
//
//   Splice echo (this exercise):
//     client socket ──splice──> pipe ──splice──> client socket
//     (0 copies through userspace — data stays in kernel buffers)
//
// How splice works
// ────────────────
// splice() moves data between a file descriptor and a pipe WITHOUT
// copying through userspace.  It manipulates kernel buffer reference
// counts instead of memcpy'ing bytes.
//
// To echo: we need TWO splices per message:
//   1. splice(socket → pipe_write)   — move data into the pipe
//   2. splice(pipe_read → socket)    — move data back out
//
// The pipe acts as a kernel-side buffer.  Data never touches our
// process address space.
//
// When to use splice
// ──────────────────
// - Proxies (splice between two sockets via a pipe)
// - File serving (splice from file fd to socket)
// - Any data forwarding where you don't need to inspect the payload
//
// NOT useful when you need to read/transform the data (encryption,
// compression, protocol parsing).
//
// New concepts
// ────────────
// - io_uring_prep_splice — async splice between fd and pipe
// - pipe2()             — create a pipe pair
// - SPLICE_F_MOVE       — hint to move (not copy) pages
// - Per-client pipe     — each client needs its own pipe buffer

#include "echo_common.hpp"

#include <sys/resource.h>

#include <coroutine>

// ─── Task + IoRequest (same as ex 12) ───────────────────────────────

struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

struct IoRequest {
  std::coroutine_handle<> handle;
  int result = 0;
};

// ─── Awaitables ─────────────────────────────────────────────────────

struct AsyncAccept : IoRequest {
  Ring &ring;
  int listen_fd;
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);

  AsyncAccept(Ring &r, int lfd) : ring(r), listen_fd(lfd) {}
  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    handle = h;
    auto *sqe = ring.get_sqe();
    io_uring_prep_accept(sqe, listen_fd, reinterpret_cast<sockaddr *>(&addr),
                         &len, 0);
    io_uring_sqe_set_data(sqe, static_cast<IoRequest *>(this));
  }
  int await_resume() { return result; }
};

struct AsyncSplice : IoRequest {
  Ring &ring;
  int fd_in, fd_out;
  unsigned len;
  unsigned flags;

  AsyncSplice(Ring &r, int in, int out, unsigned l, unsigned f = SPLICE_F_MOVE)
      : ring(r), fd_in(in), fd_out(out), len(l), flags(f) {}
  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    handle = h;
    auto *sqe = ring.get_sqe();
    // splice(fd_in, off_in, fd_out, off_out, len, flags)
    // -1 for offsets = use current file position (pipe/socket)
    io_uring_prep_splice(sqe, fd_in, -1, fd_out, -1, len, flags);
    io_uring_sqe_set_data(sqe, static_cast<IoRequest *>(this));
  }
  int await_resume() { return result; }
};

// ─── Per-client coroutine ───────────────────────────────────────────

Task client_handler(Ring &ring, int fd) {
  // Each client gets its own pipe for the kernel-side buffer
  int pipefd[2];
  if (pipe2(pipefd, 0) < 0) {
    std::cerr << "pipe2: " << strerror(errno) << "\n";
    close(fd);
    co_return;
  }
  int pipe_read = pipefd[0];
  int pipe_write = pipefd[1];

  for (;;) {
    // 1. splice: socket → pipe (read from client into pipe)
    AsyncSplice to_pipe(ring, fd, pipe_write, 4096);
    int n = co_await to_pipe;

    if (n <= 0) {
      if (n < 0 && n != -ECONNRESET)
        std::cerr << "splice in (fd " << fd << "): " << strerror(-n) << "\n";
      else if (n == 0)
        std::cout << "Client disconnected (fd " << fd << ")\n";
      break;
    }

    // 2. splice: pipe → socket (send from pipe back to client)
    int remaining = n;
    while (remaining > 0) {
      AsyncSplice from_pipe(ring, pipe_read, fd, remaining);
      int sent = co_await from_pipe;
      if (sent <= 0) {
        if (sent < 0)
          std::cerr << "splice out (fd " << fd << "): " << strerror(-sent)
                    << "\n";
        goto done;
      }
      remaining -= sent;
    }
  }

done:
  close(pipe_read);
  close(pipe_write);
  close(fd);
}

// ─── Accept loop ────────────────────────────────────────────────────

Task accept_loop(Ring &ring, int listen_fd) {
  for (;;) {
    AsyncAccept accept_op(ring, listen_fd);
    int client_fd = co_await accept_op;

    if (client_fd < 0) {
      std::cerr << "accept: " << strerror(-client_fd) << "\n";
      continue;
    }

    client_handler(ring, client_fd);
  }
}

// ─── Event loop ─────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  std::ios_base::sync_with_stdio(false);
  std::cout.tie(nullptr);

  int port = (argc > 1) ? std::stoi(argv[1]) : 9000;

  // Splice needs 3 fds per client (socket + pipe pair).
  // Raise the limit so we don't hit "too many open files".
  rlimit rl = { 65536, 65536 };
  setrlimit(RLIMIT_NOFILE, &rl);

  try {
    int listen_fd = create_listener(port);
    std::cout << "Listening on port " << port << " (splice zero-copy)\n";

    Ring ring(4096, IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN);

    accept_loop(ring, listen_fd);

    for (;;) {
      unsigned ready = ring.submit_and_wait();

      for (unsigned j = 0; j < ready; ++j) {
        auto *cqe = ring.peek();
        if (!cqe)
          break;

        auto *req = static_cast<IoRequest *>(io_uring_cqe_get_data(cqe));
        int res = cqe->res;
        ring.seen(cqe);

        req->result = res;
        req->handle.resume();
      }
    }

    close(listen_fd);

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
