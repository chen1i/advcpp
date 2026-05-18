cmake_minimum_required(VERSION 3.20)
project(userspace_drivers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_STATIC_BINARIES "Build additional fully static exercise binaries" OFF)

function(add_exercise name)
    add_executable(${name} ${name}.cpp)
    if(BUILD_STATIC_BINARIES)
        add_executable(${name}_static ${name}.cpp)
        target_link_options(${name}_static PRIVATE
            -static
            -static-libstdc++
            -static-libgcc)
    endif()
endfunction()

add_exercise(01_pci_enum)
add_exercise(02_config_space)
add_exercise(03_mmio)
add_exercise(04_bind_unbind)
