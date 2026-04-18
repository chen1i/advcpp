cmake_minimum_required(VERSION 3.20)
project(userspace_drivers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(add_exercise name)
    add_executable(${name} ${name}.cpp)
endfunction()

add_exercise(01_pci_enum)
