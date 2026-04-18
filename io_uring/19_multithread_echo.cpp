// Exercise 19: Multi-threaded io_uring Echo Server
//
// Goal: scale across CPU cores.  One accept thread distributes
// connections to N worker threads, each with its own io_uring ring.
//
// Architecture
// ────────────
//                    ┌─── worker 0 (ring, recv/send loop)
//   accept thread ───┼─── worker 1 (ring, recv/send loop)
//                    ├─── worker 2 (ring, recv/send loop)
//                    └─── ...
//
// The accept thread runs its own ring for accept only.
// New connections are assigned round-robin to workers.
// Each worker has its own ring — no sharing, no locking.
//
// How to pass an fd to another ring
// ──────────────────────────────────
// We use io_uring_prep_msg_ring to send a message from the accept
// ring to a worker ring.  The worker's CQE delivers the fd as
// user_data.  This is lock-free, zero-copy inter-ring communication.
//
// Why thread-per-ring
// ───────────────────
// - No locking on the ring (SINGLE_ISSUER)
// - Each thread has its own CQ/SQ — no false sharing
// - DEFER_TASKRUN works per-ring
// - Cache-friendly: each thread's working set stays hot
//
// New concepts
// ────────────
// - io_uring_prep_msg_ring  — send a message to another ring
// - IORING_MSG_DATA         — deliver arbitrary data via CQE
// - Thread-per-ring pattern — shared-nothing architecture
// - Round-robin connection distribution

#include "echo_common.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>

#include <array>
#include <atomic>
#include <coroutine>
#include <thread>
#include <vector>

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

struct AsyncRecv : IoRequest {
  Ring &ring;
  int fd;
  char *buf;
  unsigned len;
  AsyncRecv(Ring &r, int f, char *b, unsigned l)
      : ring(r), fd(f), buf(b), len(l) {}
  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    handle = h;
    auto *sqe = ring.get_sqe();
    io_uring_prep_recv(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, static_cast<IoRequest *>(this));
  }
  int await_resume() { return result; }
};

struct AsyncSend : IoRequest {
  Ring &ring;
  int fd;
  const char *buf;
  unsigned len;
  AsyncSend(Ring &r, int f, const char *b, unsigned l)
      : ring(r), fd(f), buf(b), len(l) {}
  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    handle = h;
    auto *sqe = ring.get_sqe();
    io_uring_prep_send(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, static_cast<IoRequest *>(this));
  }
  int await_resume() { return result; }
};

// ─── Sentinel for new-connection CQEs from msg_ring ─────────────────

// When the accept thread sends us a new fd via msg_ring, we get a
// CQE with user_data = NEW_CONN_TAG.  The fd is in cqe->res.
static constexpr __u64 NEW_CONN_TAG = 0xFFFF'FFFF'FFFF'FFFFULL;

// ─── Per-client coroutine ───────────────────────────────────────────

Task client_handler(Ring &ring, int fd) {
  std::array<char, 4096> buf{};

  for (;;) {
    AsyncRecv recv_op(ring, fd, buf.data(), buf.size());
    int n = co_await recv_op;

    if (n <= 0) {
      if (n < 0 && n != -ECONNRESET)
        std::cerr << "recv (fd " << fd << "): " << strerror(-n) << "\n";
      else if (n == 0)
        std::cout << "Client disconnected (fd " << fd << ")\n";
      break;
    }

    std::cout << "fd " << fd << ": " << std::string_view(buf.data(), n);

    AsyncSend send_op(ring, fd, buf.data(), n);
    int sent = co_await send_op;
    if (sent <= 0) {
      if (sent < 0)
        std::cerr << "send (fd " << fd << "): " << strerror(-sent) << "\n";
      break;
    }
  }

  close(fd);
}

// ─── Worker thread ──────────────────────────────────────────────────

// ─── Lock-free fd queue (SPSC) per worker ───────────────────────────
struct FdQueue {
  static constexpr int CAPACITY = 4096;
  std::array<int, CAPACITY> data{};
  std::atomic<int> head{0};
  std::atomic<int> tail{0};

  bool push(int fd) {
    int t = tail.load(std::memory_order_relaxed);
    int next = (t + 1) % CAPACITY;
    if (next == head.load(std::memory_order_acquire))
      return false; // full
    data[t] = fd;
    tail.store(next, std::memory_order_release);
    return true;
  }

  bool pop(int &fd) {
    int h = head.load(std::memory_order_relaxed);
    if (h == tail.load(std::memory_order_acquire))
      return false; // empty
    fd = data[h];
    head.store((h + 1) % CAPACITY, std::memory_order_release);
    return true;
  }
};

struct Worker {
  std::thread thread;
  FdQueue queue;
  int eventfd_val = -1; // to wake the worker
};

static void run_worker(int id, Ring &ring, FdQueue &queue, int efd) {
  // The event loop processes:
  // 1. eventfd readiness → drain the fd queue, spawn client coroutines
  // 2. recv/send CQEs → resume coroutines

  // Submit a poll on the eventfd to get notified of new connections
  auto submit_poll = [&]() {
    auto *sqe = ring.get_sqe();
    io_uring_prep_poll_add(sqe, efd, POLLIN);
    io_uring_sqe_set_data64(sqe, NEW_CONN_TAG);
  };

  submit_poll();

  for (;;) {
    unsigned ready = ring.submit_and_wait();

    for (unsigned j = 0; j < ready; ++j) {
      auto *cqe = ring.peek();
      if (!cqe)
        break;

      __u64 ud = cqe->user_data;
      int res = cqe->res;
      ring.seen(cqe);

      if (ud == NEW_CONN_TAG) {
        // eventfd fired — drain the queue
        if (res > 0) {
          uint64_t val;
          read(efd, &val, sizeof(val));

          int fd;
          while (queue.pop(fd)) {
            std::cout << "Worker " << id << ": client connected (fd " << fd
                      << ")\n";
            client_handler(ring, fd);
          }
        }

        // Re-arm the poll
        submit_poll();
      } else {
        // Coroutine CQE
        auto *req = reinterpret_cast<IoRequest *>(ud);
        req->result = res;
        req->handle.resume();
      }
    }
  }
}

// ─── main: accept thread ────────────────────────────────────────────

int main(int argc, char *argv[]) {
  std::ios_base::sync_with_stdio(false);
  std::cout.tie(nullptr);

  int port = (argc > 1) ? std::stoi(argv[1]) : 9000;
  unsigned num_workers = std::thread::hardware_concurrency();
  if (num_workers == 0)
    num_workers = 4;
  // Leave one core for the accept thread
  if (num_workers > 1)
    --num_workers;

  rlimit rl = {65536, 65536};
  setrlimit(RLIMIT_NOFILE, &rl);

  try {
    int listen_fd = create_listener(port);
    std::cout << "Listening on port " << port << " (" << num_workers
              << " worker threads)\n";

    // Create workers
    std::vector<Worker> workers(num_workers);
    std::vector<Ring *> rings;

    for (unsigned i = 0; i < num_workers; ++i) {
      int efd = eventfd(0, EFD_NONBLOCK);
      if (efd < 0)
        throw std::system_error(errno, std::system_category(), "eventfd");
      workers[i].eventfd_val = efd;
    }

    // Start worker threads
    for (unsigned i = 0; i < num_workers; ++i) {
      workers[i].thread = std::thread([i, &workers]() {
        Ring ring(4096,
                  IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN);
        run_worker(i, ring, workers[i].queue, workers[i].eventfd_val);
      });
      workers[i].thread.detach();
    }

    // Accept loop on main thread using io_uring
    Ring ring(4096, IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN);

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    auto *sqe = ring.get_sqe();
    io_uring_prep_accept(sqe, listen_fd,
                         reinterpret_cast<sockaddr *>(&client_addr),
                         &client_len, 0);
    io_uring_sqe_set_data64(sqe, 0);

    unsigned next_worker = 0;

    for (;;) {
      unsigned ready = ring.submit_and_wait();

      for (unsigned j = 0; j < ready; ++j) {
        auto *cqe = ring.peek();
        if (!cqe)
          break;

        int res = cqe->res;
        ring.seen(cqe);

        if (res >= 0) {
          int client_fd = res;

          // Round-robin to workers
          auto &w = workers[next_worker % num_workers];
          w.queue.push(client_fd);

          // Wake the worker via eventfd
          uint64_t val = 1;
          write(w.eventfd_val, &val, sizeof(val));

          next_worker++;
        }

        // Re-arm accept
        auto *accept_sqe = ring.get_sqe();
        io_uring_prep_accept(accept_sqe, listen_fd,
                             reinterpret_cast<sockaddr *>(&client_addr),
                             &client_len, 0);
        io_uring_sqe_set_data64(accept_sqe, 0);
      }
    }

    close(listen_fd);

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
