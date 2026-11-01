cmake_minimum_required(VERSION 3.16)
project(MultithreadedWebServer VERSION 1.0.0 LANGUAGES CXX)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type options
option(ENABLE_ASAN "Enable AddressSanitizer for memory error detection" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer for data race detection" OFF)
option(DEBUG_BUILD "Enable debug build with symbols" OFF)
option(BUILD_TESTS "Build unit and integration tests" ON)

# Compiler-specific warning and optimization options
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    if(DEBUG_BUILD)
        add_compile_options(-g -O0 -fno-omit-frame-pointer)
    else()
        add_compile_options(-O3 -march=native -fno-omit-frame-pointer)
    endif()
    
    if(ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address -fsanitize=undefined)
        set(CMAKE_BUILD_TYPE Debug)
    endif()
    
    if(ENABLE_TSAN)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
        set(CMAKE_BUILD_TYPE Debug)
    endif()
endif()

# Include directories
include_directories(include)

# Find POSIX threads
find_package(Threads REQUIRED)

# Core library source files (everything except main.cpp)
set(SERVER_CORE_SOURCES
    src/epoll_wrapper.cpp
    src/thread_pool.cpp
    src/http_request.cpp
    src/http_response.cpp
    src/file_handler.cpp
    src/cache.cpp
    src/rate_limiter.cpp
    src/logger.cpp
    src/server.cpp
)

# Static library for server core logic
add_library(server_core STATIC ${SERVER_CORE_SOURCES})
target_link_libraries(server_core PUBLIC Threads::Threads)
target_include_directories(server_core PUBLIC include)

# Main web server binary
add_executable(webserver src/main.cpp)
target_link_libraries(webserver PRIVATE server_core)

# Set output directory
set_target_properties(webserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Copy public assets directory into build tree for testing
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/public/ DESTINATION ${CMAKE_BINARY_DIR}/public)

# Testing configuration
if(BUILD_TESTS)
    enable_testing()

    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GTest not found locally. Fetching GoogleTest v1.14.0...")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    file(GLOB TEST_SOURCES "tests/*.cpp")
    add_executable(webserver_tests ${TEST_SOURCES})
    target_link_libraries(webserver_tests PRIVATE server_core gtest gtest_main)
    target_include_directories(webserver_tests PRIVATE include)

    set_target_properties(webserver_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    include(GoogleTest)
    gtest_discover_tests(webserver_tests)
endif()
