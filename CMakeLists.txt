cmake_minimum_required(VERSION 3.20)
project(advcpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Dependencies ────────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)

cmake_policy(SET CMP0167 NEW)
find_package(Boost)
find_package(Threads REQUIRED)

# ─── Helper: one executable per .cpp, linked against liburing ────────

function(add_exercise name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE PkgConfig::URING)
endfunction()

# ─── File I/O exercises (01–06) ──────────────────────────────────────

add_exercise(01_basic_read)
add_exercise(02_batch_read)
add_exercise(03_file_copy)
add_exercise(04_linked_copy)
add_exercise(05_pipeline_copy)
add_exercise(06_fixed_buffers)

# ─── Echo servers & bench (07–11) ────────────────────────────────────

add_exercise(07_echo_server)
add_exercise(07_echo_server_uring_naive)
add_exercise(07_echo_server_epoll)
add_exercise(08_echo_bench)
add_exercise(09_echo_sqpoll)
add_exercise(10_provided_buffers)
add_exercise(11_multishot)

# ─── Coroutine exercises (12–15) ─────────────────────────────────────

add_exercise(12_coroutine_echo)
add_exercise(13_file_server)
add_exercise(14_timeout)
add_exercise(15_graceful_shutdown)

# ─── Advanced exercises (17–19) ──────────────────────────────────────

add_exercise(17_splice_echo)
add_exercise(18_registered_fds)

add_exercise(19_multithread_echo)
target_link_libraries(19_multithread_echo PRIVATE Threads::Threads)

# ─── Boost.Asio exercise (16) ───────────────────────────────────────

if(Boost_FOUND)
    add_exercise(16_asio_echo)
    target_link_libraries(16_asio_echo PRIVATE
        Boost::boost Threads::Threads)
    target_compile_definitions(16_asio_echo PRIVATE
        BOOST_ASIO_HAS_IO_URING)
else()
    message(STATUS "Boost not found — skipping 16_asio_echo")
endif()
