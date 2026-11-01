# High-Performance Event-Driven HTTP Web Server (C++17)

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Linux epoll](https://img.shields.io/badge/I%2FO-epoll%20(Edge--Triggered)-orange.svg)](https://man7.org/linux/man-pages/man7/epoll.7.html)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Unit & Integration Tests](https://img.shields.io/badge/tests-42%2F42%20passing-brightgreen.svg)]()
[![AddressSanitizer](https://img.shields.io/badge/ASan%20%2F%20UBSan-clean-success.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

A production-grade, event-driven, multithreaded HTTP/1.1 web server implemented in modern C++17 for Linux. Built with non-blocking POSIX sockets, Edge-Triggered (`EPOLLET`) I/O multiplexing, atomic thread synchronization, and zero-allocation buffer pipelines. Designed to demonstrate the low-latency systems engineering and Linux kernel networking principles required for high-throughput packet processing environments (such as **Cisco Silicon One** software engineering).

---

## 1. Project Overview

Modern high-performance network systems—from cloud web gateways to carrier-class silicon operating systems—require concurrent connection scalability far beyond the capability of classic process-per-connection or thread-per-connection architectures. 

This project implements a hybrid **Reactor Pattern** decoupled from an asynchronous **Worker Thread Pool**:
- The **I/O Reactor** leverages Linux `epoll` in **Edge-Triggered (`EPOLLET`)** mode with `EPOLLONESHOT` to handle tens of thousands of active client file descriptors with $O(1)$ event signaling.
- The **Worker Thread Pool** processes HTTP/1.1 protocol parsing, URI routing, LRU caching, and socket streaming concurrently across CPU cores without blocking the event loop.
- **Empirical Throughput**: Serves up to **143,549 requests/second** on dynamic JSON workloads and **61,801 requests/second (243.89 MB/s)** on static file transfers with sub-millisecond median latency ($p50 < 600\,\mu\text{s}$).

---

## 2. Problem Statement

Classic multi-threaded server architectures allocate an operating system thread per connected client (`Thread-per-Connection`). Under high concurrency ($10^4$ to $10^5$ simultaneous connections):
1. **Thread Overhead**: Kernel stack allocations ($8\,\text{MB}$ default per thread on Linux) exhaust memory.
2. **Context-Switch Saturation**: The Linux CFS (Completely Fair Scheduler) spends more CPU cycles thrashing registers and invalidating translation lookaside buffers (TLBs) than executing userland logic.
3. **The C10K / C100K Problem**: A single slow client transmitting bytes at 1 byte/sec can pin an entire worker thread, making the server vulnerable to thread-pool starvation and Slowloris denial-of-service attacks.

### Solution
Replace thread-per-connection with non-blocking sockets and asynchronous event multiplexing via Linux `epoll(7)`. Decouple network packet ingestion from business logic so that a fixed number of threads can process arbitrarily large connection volumes.

---

## 3. Architecture

The server adopts a decoupled Reactor + Thread Pool pipeline:

```
                            [ Linux Kernel ]
                                   |
                         Incoming TCP Packets
                                   v
                       +-----------------------+
                       | epoll Reactor Thread  |
                       |  (Edge-Triggered ET)  |
                       +-----------------------+
                                   |
                +------------------+------------------+
                |                                     |
                v                                     v
       [ Listening Socket ]                  [ Client Sockets ]
     - accept4() until EAGAIN              - recv() until EAGAIN
     - SOCK_NONBLOCK | SOCK_CLOEXEC        - Buffer accumulation
     - TCP_NODELAY, SO_REUSEPORT           - Detect complete HTTP req
                                                      |
                                                      v
                                        +---------------------------+
                                        |  Synchronized Task Queue  |
                                        |  (Mutex + Cond Variable)  |
                                        +---------------------------+
                                                      |
                                   +------------------+------------------+
                                   |                                     |
                                   v                                     v
                          +-----------------+                   +-----------------+
                          | Worker Thread 1 |                   | Worker Thread N |
                          | - HTTP Parser   |                   | - HTTP Parser   |
                          | - LRU Cache Hit |                   | - LRU Cache Hit |
                          | - Non-blocking  |                   | - Non-blocking  |
                          |   Async Write   |                   |   Async Write   |
                          +-----------------+                   +-----------------+
```

### Architectural Diagrams
The repository contains comprehensive architectural views in the `images/` directory:
- [Main Architecture](images/Main%20Architecture.png): Complete end-to-end request pipeline.
- [Component Layer Architecture](images/Architecture%20Components.png): Layered separation of Client, Network, Threading, Application, and Data tiers.
- [Request Processing Flow](images/Architecture%20Flow.png): Detailed state flowchart showing timings and error handling.

For a deeper dive into memory layouts, kernel event demuxing, and packet flows, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## 4. Technical Design Decisions

For exhaustive trade-off analyses, see [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md). Summary of core decisions:

1. **Edge-Triggered (`EPOLLET`) vs. Level-Triggered (LT)**:
   - ET reduces redundant `epoll_wait` wakeups and avoids event storming on partially drained buffers.
   - Requires non-blocking reads/writes wrapped in `while (true)` loops until `errno == EAGAIN || errno == EWOULDBLOCK`.
2. **Thread Safety with `EPOLLONESHOT`**:
   - Prevents race conditions where two threads wake up on the same file descriptor simultaneously.
   - Once an event is serviced, the socket is explicitly re-armed via `epoll_ctl(..., EPOLL_CTL_MOD, ...)`.
3. **Deadlock-Free Two-Level Lock Hierarchy**:
   - *Level 1*: `connections_mutex_` protects the active connection map.
   - *Level 2*: `Connection::mutex_` protects per-socket buffers and state.
   - *Strict Invariant*: Code holding `Connection::mutex_` is never permitted to acquire `connections_mutex_`, eliminating ABBA deadlocks.
4. **Instantaneous Graceful Wakeup via Linux `eventfd`**:
   - Server shutdown writes an 8-byte counter to an `eventfd` monitored by epoll.
   - Eliminates polling loops and terminates cleanly with zero millisecond delay.
5. **RFC 7230 / RFC 7231 Protocol Conformance**:
   - Mandatory `Host` header enforced for HTTP/1.1 (RFC 7230 §5.4); invalid requests return `400 Bad Request`.
   - HEAD method executes the exact GET pipeline and returns identical `Content-Length` / `Content-Type` headers, but suppresses entity body bytes from the wire (RFC 7231 §4.3.2).

---

## 5. Technology Stack

- **Language**: Modern C++17 (`-std=c++17 -Wall -Wextra -Wpedantic -Wshadow`)
- **Platform**: Linux (Ubuntu 22.04 / 24.04 LTS x86_64)
- **Networking APIs**: Linux POSIX Sockets, `epoll(7)`, `eventfd(2)`, `accept4(2)`, `fcntl(2)`
- **Concurrency**: POSIX Threads (`std::thread`), `std::mutex`, `std::condition_variable`, `std::atomic`
- **Build System**: CMake 3.16+
- **Unit Testing**: GoogleTest v1.14.0 (automatically fetched via CMake FetchContent)
- **Benchmarking Tools**: `wrk 4.2.0 [epoll]`, `curl`, `ss`, `netstat`
- **Verification**: GCC AddressSanitizer (`-fsanitize=address,undefined`), GDB

---

## 6. Features

- **Non-blocking Edge-Triggered Networking**: High-efficiency event handling with `EPOLLET` and `accept4`.
- **Worker Thread Pool**: Dynamic task queue supporting arbitrary callable tasks using `std::invoke_result_t`.
- **HTTP/1.1 Request/Response Engine**:
  - Full request parser with method, path, query parameters, header normalization, and body parsing.
  - Supports `GET` and `HEAD` methods.
  - Returns appropriate status codes: `200 OK`, `204 No Content`, `301 Moved`, `304 Not Modified`, `400 Bad Request`, `403 Forbidden`, `404 Not Found`, `405 Method Not Allowed`, `413 Payload Too Large`, `500 Internal Error`, `503 Service Unavailable`.
  - HTTP/1.1 persistent connections (`Connection: keep-alive` and `Connection: close`).
- **Memory & Resource Protection**:
  - Strict connection limit enforcement (`max_connections` configurable in `config.json`).
  - Max request buffer cap ($64\,\text{KB}$) to prevent memory exhaustion attacks.
  - Connection idle timeout reclamation (auto-cleans inactive sockets after $30\,\text{s}$).
- **Thread-Safe LRU File Cache**:
  - High-speed in-memory cache with configurable memory bounds (e.g. $100\,\text{MB}$) and TTL expiration.
- **Token Bucket Rate Limiting**:
  - Per-IP rate limiting defending against burst floods and abusive clients.
- **Structured Logging**:
  - Separate Nginx-format access log (`logs/access.log`) and error log (`logs/error.log`) with log levels.
- **Observability REST API**:
  - `/api/info` and `/api/status` expose real-time thread count, active connections, queue depth, cache hit ratio, and request counters.

---

## 7. Directory Structure

```
multithreaded-webserver-cpp/
├── CMakeLists.txt            # Modern CMake build system with FetchContent for GTest
├── config.json               # Server configuration (port, threads, max connections)
├── include/                  # Public header files
│   ├── cache.h               # Thread-safe LRU cache with TTL expiration
│   ├── epoll_wrapper.h       # Linux epoll C++ wrapper with eventfd wakeup
│   ├── file_handler.h        # Static file serving, path traversal defense, MIME types
│   ├── http_request.h        # HTTP/1.1 request parser (RFC 7230 compliant)
│   ├── http_response.h       # HTTP/1.1 response builder with HEAD body suppression
│   ├── logger.h              # Thread-safe structured file and console logger
│   ├── rate_limiter.h        # Token bucket rate limiting engine
│   ├── server.h              # Edge-triggered Server reactor & lifecycle manager
│   └── thread_pool.h         # Modern C++17 thread pool with task queue
├── src/                      # Implementation files
│   ├── cache.cpp
│   ├── epoll_wrapper.cpp
│   ├── file_handler.cpp
│   ├── http_request.cpp
│   ├── http_response.cpp
│   ├── logger.cpp
│   ├── main.cpp              # Server entry point & signal handling
│   ├── rate_limiter.cpp
│   ├── server.cpp
│   └── thread_pool.cpp
├── tests/                    # Unit and integration test suite
│   ├── test_cache.cpp
│   ├── test_http_request.cpp
│   ├── test_http_response.cpp
│   ├── test_main.cpp
│   ├── test_rate_limiter.cpp
│   ├── test_server_integration.cpp # Concurrent clients, slowloris, HEAD, keepalive
│   └── test_thread_pool.cpp
├── scripts/                  # Performance and benchmark utilities
│   ├── benchmark.sh          # Full benchmark suite against Nginx/Apache
│   ├── quick_bench.sh        # Developer benchmark testing all endpoints
│   └── stress_test.sh        # High-concurrency stress test script
├── public/                   # Static document root
│   ├── index.html            # Web server homepage
│   └── test.html             # Benchmark static file
├── results/                  # Benchmark performance reports
├── images/                   # Architecture diagrams and system flowcharts
├── ARCHITECTURE.md           # Systems architecture & packet flow deep dive
├── DESIGN_DECISIONS.md       # Engineering trade-offs and rationale
└── BENCHMARKS.md             # Benchmark numbers, latency percentiles, and analysis
```

---

## 8. Installation Instructions for Ubuntu

### Prerequisites
Install the required C++ build toolchain:

```bash
sudo apt update
sudo apt install -y build-essential cmake git curl
```

*(Optional: GoogleTest is fetched and built automatically via CMake `FetchContent`, eliminating manual library installations).*

---

## 9. Build and Execution Instructions

### Standard Release Build (Optimized)
```bash
# Clone the repository
git clone https://github.com/swe-robertkibet/multithreaded-webserver-cpp.git
cd multithreaded-webserver-cpp

# Configure and compile with Release optimizations (-O3 -march=native)
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Running the Server
```bash
# Run server on default port 8080 (auto-detects hardware threads)
./bin/webserver 8080

# Or run on port 9090 with 8 dedicated worker threads
./bin/webserver 9090 8
```

### Debug Build with AddressSanitizer (ASan)
To verify memory safety and ensure zero memory leaks:
```bash
mkdir -p build_debug && cd build_debug
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DBUILD_TESTS=ON
make -j$(nproc)
./bin/webserver_tests
```

---

## 10. API Usage Examples

Test the running server using `curl`:

### 1. Static HTML File (`GET /`)
```bash
curl -i http://127.0.0.1:8080/
```
*Response*:
```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: 3912
Connection: keep-alive
Server: MultithreadedWebServer/1.0
Date: Sun, 01 Nov 2026 15:23:24 GMT

<!DOCTYPE html>...
```

### 2. HEAD Request Body Suppression (`HEAD /`)
```bash
curl -i --head http://127.0.0.1:8080/
```
*Response* (identical headers, zero body bytes transmitted):
```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: 3912
Connection: keep-alive
Server: MultithreadedWebServer/1.0
Date: Sun, 01 Nov 2026 15:23:24 GMT
```

### 3. Server Metrics & Statistics (`GET /api/info`)
```bash
curl -s http://127.0.0.1:8080/api/info | jq .
```
*Sample JSON Response*:
```json
{
  "server": "MultithreadedWebServer/1.0",
  "engine": "C++17 Epoll Edge-Triggered Reactor",
  "timestamp": "2026-11-01T15:23:24Z",
  "thread_pool_size": 8,
  "queue_size": 0,
  "active_tasks": 0,
  "active_connections": 1,
  "max_connections": 10000,
  "total_requests": 619772,
  "document_root": "./public/",
  "cache": {
    "hits": 619720,
    "misses": 1,
    "entries": 2,
    "memory_usage_bytes": 5120,
    "hit_ratio_percent": 100.0
  }
}
```

---

## 11. Benchmarking Methodology

Benchmarks are executed using `wrk 4.2.0 [epoll]` over the Linux loopback interface to isolate server performance from physical Ethernet latency.

### Test Matrix
- **Concurrency**: 100 to 500 concurrent connections.
- **Worker Threads**: 4 to 8 threads.
- **Duration**: 10 seconds sustained load per run.
- **Metrics Collected**: Total throughput (requests/sec), network transfer rate (MB/sec), and precise latency percentiles ($p50$, $p75$, $p90$, $p99$).

---

## 12. Actual Benchmark Results

The following numbers represent **actual measured results** from our empirical runs:

### Static File (`index.html`, 3.9KB) @ 100 Connections
```
Running 10s test @ http://127.0.0.1:8080/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.61ms  759.51us  18.87ms   79.49%
    Req/Sec    15.55k     1.35k   19.70k    77.25%
  Latency Distribution
     50%    1.45ms
     75%    1.87ms
     90%    2.50ms
     99%    4.39ms
  619,771 requests in 10.03s, 2.39GB read
Requests/sec:  61,801.37
Transfer/sec:    243.89MB
```

### Dynamic API Endpoint (`/api/info` JSON) @ 100 Connections
```
Running 10s test @ http://127.0.0.1:8080/api/info
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   665.49us  342.59us  10.03ms   77.83%
    Req/Sec    36.10k     4.63k   62.13k    74.50%
  Latency Distribution
     50%  599.00us
     75%  816.00us
     90%    1.07ms
     99%    1.84ms
  1,436,931 requests in 10.01s, 0.88GB read
Requests/sec: 143,549.21
Transfer/sec:     90.05MB
```

For full comparisons across different connection levels, see [BENCHMARKS.md](BENCHMARKS.md).

---

## 13. Testing Instructions

The test suite covers both unit tests (isolated classes) and integration tests (actual network interactions on live sockets).

### Running All Tests
```bash
cd build
./bin/webserver_tests
```

### Test Coverage Highlights (42 Tests Total):
- **`CacheTest`**: Basic put/get, miss detection, TTL expiration, LRU eviction order, memory bounds.
- **`HttpRequestTest`**: Request line parsing, URL decoding, header case-insensitivity, query string parameters, keep-alive detection, Host header validation.
- **`HttpResponseTest`**: Status code formatting, MIME type detection, custom headers, file response builders.
- **`RateLimiterTest`**: Burst capacity allowance, token refill math, multiple IP tracking, statistics resets.
- **`ThreadPoolTest`**: Multithreaded execution, concurrent task limits, exception propagation, queue size tracking, graceful drain.
- **`ServerIntegrationTest`**:
  - `MultipleConcurrentClients`: 30 concurrent threads sending requests simultaneously.
  - `HeadMethodCompliance`: RFC 7231 header preservation and wire body suppression.
  - `InvalidAndMalformedRequests`: Rejection of malformed syntax, missing Host headers, and unsupported methods.
  - `PartialRequestTransmission`: Slowloris-style fragmented packet transmission across multiple epoll edges.
  - `ClientDisconnectionMidRequest`: Abrupt client socket termination without resource leaks.
  - `PersistentConnectionKeepAlive`: Multiple sequential HTTP requests over a single TCP connection.
  - `GracefulShutdownUnderLoad`: Server stops cleanly under active traffic without leaking ports or hanging.

---

## 14. Known Limitations

1. **HTTP/2 & HTTP/3 (QUIC)**: Currently limited to HTTP/1.0 and HTTP/1.1 over TCP. Multiplexed binary streams (HTTP/2) and UDP-based QUIC are not yet implemented.
2. **TLS / HTTPS**: Operates over unencrypted TCP. In enterprise production deployments, TLS termination is typically offloaded to an upstream hardware gateway or Nginx/Envoy reverse proxy.
3. **Single Event Loop Thread**: While the worker pool handles CPU-bound requests across all cores, I/O multiplexing is driven by a single reactor thread. For machines with >32 cores, multi-reactor architectures (`SO_REUSEPORT` with thread-per-core event loops) would yield higher packet rates.

---

## 15. Future Improvements

1. **Zero-Copy File Transfer (`sendfile(2)`)**:
   - Stream static files directly from the page cache to the network socket descriptor using `sendfile()`, avoiding user-space memory copying.
2. **Scatter-Gather I/O (`writev(2)`)**:
   - Transmit HTTP status headers and file bodies in a single syscall via vectorized I/O.
3. **Multi-Reactor Thread-Per-Core Model**:
   - Run an independent epoll reactor thread per CPU core with `SO_REUSEPORT` socket listeners, eliminating task queue mutex contention.
4. **io_uring Integration**:
   - Implement an alternative Linux `io_uring` backend for true asynchronous kernel-ring completion processing.
5. **OpenSSL / BoringSSL Integration**:
   - Direct support for TLS 1.3 with ALPN negotiation.
