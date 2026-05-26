cmake_minimum_required(VERSION 3.20)
project(dpdk_exercises LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK QUIET IMPORTED_TARGET libdpdk)

if(NOT DPDK_FOUND)
    message(STATUS "DPDK libdpdk.pc not found; skipping DPDK exercise targets")
    message(STATUS "Install DPDK development files or set PKG_CONFIG_PATH to the directory containing libdpdk.pc")
else()
    message(STATUS "Found DPDK ${DPDK_VERSION}")
    find_file(DPDK_STATIC_ETHDEV_ARCHIVE
        NAMES librte_ethdev.a
        PATHS ${DPDK_STATIC_LIBRARY_DIRS} ${DPDK_LIBRARY_DIRS}
        NO_DEFAULT_PATH)

    set(DPDK_MINIMAL_STATIC_LIBS
        net_virtio
        bus_pci
        ethdev
        pci
        net
        mbuf
        mempool
        ring
        rcu
        eal
        pmu
        telemetry
        argparse
        kvargs
        log)

    set(DPDK_MINIMAL_STATIC_ARCHIVES)
    set(DPDK_MINIMAL_STATIC_MISSING)
    foreach(lib IN LISTS DPDK_MINIMAL_STATIC_LIBS)
        string(TOUPPER "${lib}" lib_upper)
        find_file(DPDK_STATIC_${lib_upper}_ARCHIVE
            NAMES librte_${lib}.a
            PATHS ${DPDK_STATIC_LIBRARY_DIRS} ${DPDK_LIBRARY_DIRS}
            NO_DEFAULT_PATH)
        if(DPDK_STATIC_${lib_upper}_ARCHIVE)
            list(APPEND DPDK_MINIMAL_STATIC_ARCHIVES
                "${DPDK_STATIC_${lib_upper}_ARCHIVE}")
        else()
            list(APPEND DPDK_MINIMAL_STATIC_MISSING "librte_${lib}.a")
        endif()
    endforeach()

    function(add_dpdk_exercise name)
        add_executable(${name} ${name}.cpp)
        target_link_libraries(${name} PRIVATE PkgConfig::DPDK)

        if(DPDK_STATIC_ETHDEV_ARCHIVE AND NOT DPDK_MINIMAL_STATIC_MISSING)
            add_executable(${name}_static ${name}.cpp)
            target_sources(${name}_static PRIVATE dpdk_static_compat.cpp)
            target_include_directories(${name}_static PRIVATE
                ${DPDK_STATIC_INCLUDE_DIRS})
            target_compile_options(${name}_static PRIVATE
                ${DPDK_STATIC_CFLAGS_OTHER})
            target_link_libraries(${name}_static PRIVATE
                -Wl,-u,rte_pci_bus
                -Wl,-u,this_pmd_namenet_virtio
                -Wl,--start-group
                ${DPDK_MINIMAL_STATIC_ARCHIVES}
                -Wl,--end-group
                atomic
                pthread
                m
                dl)
            target_link_options(${name}_static PRIVATE
                -static
                -static-libstdc++
                -static-libgcc)
        endif()
    endfunction()

    add_dpdk_exercise(01_env_check)
    add_dpdk_exercise(02_pci_probe)

    if(DPDK_STATIC_ETHDEV_ARCHIVE AND NOT DPDK_MINIMAL_STATIC_MISSING)
        message(STATUS "Found minimal DPDK static archives; static exercise targets are enabled")
    elseif(DPDK_STATIC_ETHDEV_ARCHIVE)
        message(STATUS "DPDK static archives were found, but minimal static targets are disabled")
        message(STATUS "Missing archives: ${DPDK_MINIMAL_STATIC_MISSING}")
    else()
        message(STATUS "DPDK static archives were not found; *_static targets are disabled")
        message(STATUS "Install the DPDK static libraries package or build DPDK with static libraries enabled")
    endif()
endif()
