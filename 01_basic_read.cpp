// Exercise 01: Basic io_uring Read — C++ edition
//
// Same 8-step workflow, but now with C++ idioms:
//
//   - RAII wrappers      → no manual close()/queue_exit(), no leak on error
//   - std::system_error  → errors carry context, no fprintf scattered around
//   - std::array         → sized buffer on the stack
//   - std::string_view   → zero-copy view of the bytes we read

#include "io_uring_utils.hpp"

#include <array>
#include <iostream>
#include <string_view>

// ─── main ───────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    const char* path = (argc > 1) ? argv[1] : "sample.txt";

    try {
        FileDescriptor file(path, O_RDONLY);              // RAII open
        Ring ring(/*queue_depth=*/4);                     // RAII ring init

        // Prepare a read SQE
        auto* sqe = ring.get_sqe();
        std::array<char, 4096> buf{};
        io_uring_prep_read(sqe, file.get(), buf.data(), buf.size(), /*offset=*/0);
        io_uring_sqe_set_data64(sqe, file.get());

        // Submit & wait
        int n = ring.submit();
        std::cout << "Submitted " << n << " SQE(s)\n";

        auto* cqe = ring.wait();

        if (cqe->res < 0) {
            throw std::system_error(-cqe->res, std::system_category(), "read");
        }

        // Zero-copy view over the filled portion of buf
        std::string_view result(buf.data(), cqe->res);
        std::cout << "Read " << result.size() << " bytes:\n" << result;

        ring.seen(cqe);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    // FileDescriptor and Ring destructors handle cleanup automatically —
    // even if an exception is thrown mid-way through.

    return 0;
}
