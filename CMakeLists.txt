cmake_minimum_required(VERSION 3.22)
project(cache_allocator_sim
        VERSION 1.0.0
        DESCRIPTION "Parameterized L1/L2 Cache Simulator with Custom User-Space Memory Allocator"
        LANGUAGES CXX)

# ----------------------------------------------------------------------------
# Global project settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(CAS_BUILD_TESTS "Build GoogleTest unit test suite" ON)
option(CAS_BUILD_BENCHMARKS "Build Google Benchmark micro-benchmarks" ON)
option(CAS_BUILD_DEMO "Build the demo/driver executable" ON)
option(CAS_ENABLE_SANITIZERS "Enable ASan/UBSan in debug builds" OFF)
option(CAS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ----------------------------------------------------------------------------
# Common compiler warnings / hardening flags
# ----------------------------------------------------------------------------
add_library(cas_project_options INTERFACE)
target_compile_features(cas_project_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(cas_project_options INTERFACE /W4 /permissive-)
    if(CAS_WARNINGS_AS_ERRORS)
        target_compile_options(cas_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(cas_project_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
    if(CAS_WARNINGS_AS_ERRORS)
        target_compile_options(cas_project_options INTERFACE -Werror)
    endif()
endif()

if(CAS_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(cas_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cas_project_options INTERFACE -fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
# Core library: cache simulator + pool allocator + utilities + profiler bridge
# ----------------------------------------------------------------------------
add_library(cache_allocator_core
    src/cache/cache.cpp
    src/cache/eviction_policy.cpp
    src/allocator/pool_allocator.cpp
    src/utils/trace_parser.cpp
    src/utils/workload_generator.cpp
    src/profiler/cache_allocator_bridge.cpp
)
add_library(cas::core ALIAS cache_allocator_core)

target_include_directories(cache_allocator_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cache_allocator_core
    PUBLIC cas_project_options Threads::Threads
)

# ----------------------------------------------------------------------------
# Demo driver executable
# ----------------------------------------------------------------------------
if(CAS_BUILD_DEMO)
    add_executable(cas_demo src/main.cpp)
    target_link_libraries(cas_demo PRIVATE cas::core)
endif()

# ----------------------------------------------------------------------------
# GoogleTest
# ----------------------------------------------------------------------------
if(CAS_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    # Keep gtest's own runtime linkage consistent on MSVC; irrelevant elsewhere.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(cas_tests
        tests/test_cache.cpp
        tests/test_allocator.cpp
    )
    target_link_libraries(cas_tests PRIVATE cas::core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(cas_tests)
endif()

# ----------------------------------------------------------------------------
# Google Benchmark
# ----------------------------------------------------------------------------
if(CAS_BUILD_BENCHMARKS)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(cas_benchmarks benchmarks/bench_allocator.cpp)
    target_link_libraries(cas_benchmarks PRIVATE cas::core benchmark::benchmark)
endif()

# ----------------------------------------------------------------------------
# Install rules (library + headers)
# ----------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS cache_allocator_core
        EXPORT cache_allocator_sim_targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
