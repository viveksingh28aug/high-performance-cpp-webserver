# System Architecture & Internals

This document details the high-performance architectural design, Linux kernel event notification model, threading hierarchy, and memory management strategies of the **Multithreaded Edge-Triggered HTTP Web Server**. This architecture is specifically engineered to demonstrate principles relevant to high-throughput, low-latency networking software (such as Cisco Silicon One software ecosystems).

---

## 1. High-Level Architectural Pattern: Reactor with Worker Thread Pool

The server implements a hybrid **Reactor Pattern** decoupled from an asynchronous **Worker Thread Pool**:

```
                       +----------------------------------+
                       |      Linux Kernel Subsystem      |
                       +----------------------------------+
                                        |
                            Incoming TCP Connections
                                        v
                       +----------------------------------+
                       |     epoll Instance (epoll_wait)  |
                       |      Edge-Triggered (EPOLLET)    |
                       +----------------------------------+
                                        |
               +------------------------+------------------------+
               | Event Dispatch Loop (Event Reactor Thread)      |
               +-------------------------------------------------+
                         /              |               \
                        /               |                \
         EPOLLIN on listen_fd    EPOLLIN on conn_fd    EPOLLOUT on conn_fd
                   |                    |                        |
                   v                    v                        v
         +------------------+  +------------------+    +-------------------+
         |  handle_accept() |  |handle_client_data|    |handle_client_write|
         |  accept4() loop  |  |  non-blocking    |    |  non-blocking     |
         |  until EAGAIN    |  |  recv() loop     |    |  send() loop      |
         +------------------+  +------------------+    +-------------------+
                                        |
                               Complete HTTP Request
                                        v
                       +----------------------------------+
                       |  Thread-Safe Task Queue (FIFO)   |
                       |  Mutex + Condition Variable      |
                       +----------------------------------+
                                        |
            +---------------------------+---------------------------+
            |                           |                           |
            v                           v                           v
   +-----------------+         +-----------------+         +-----------------+
   | Worker Thread 1 |         | Worker Thread 2 |         | Worker Thread N |
   |  - HTTP Parser  |         |  - HTTP Parser  |         |  - HTTP Parser  |
   |  - Router & LRU |         |  - Router & LRU |         |  - Router & LRU |
   |  - Async Send   |         |  - Async Send   |         |  - Async Send   |
   +-----------------+         +-----------------+         +-----------------+
```

### Decoupling I/O Multiplexing from Protocol Execution
1. **Event Reactor Thread**:
   - Solely responsible for driving the Linux `epoll` event loop.
   - Handles connection acceptance (`accept4`), stream reception (`recv` into connection buffers), and write re-arming (`EPOLLOUT`).
   - Never blocks on disk I/O, file parsing, or complex payload generation.
2. **Worker Thread Pool**:
   - A configurable pool of persistent worker threads (`std::thread::hardware_concurrency()` by default).
   - Consumes completed HTTP requests from a synchronized task queue.
   - Executes HTTP/1.1 parsing, file reading, LRU caching, REST API JSON rendering, and initial non-blocking transmission.

---

## 2. Linux Kernel & Socket Layer Subsystems

### Edge-Triggered (`EPOLLET`) Semantics
Unlike Level-Triggered (LT) mode—which continuously wakes `epoll_wait` as long as data remains in the socket buffer—**Edge-Triggered (ET)** mode only delivers an event notification on a state transition (an "edge"):
- **Listening Socket (`server_fd_`)**: Registered with `EPOLLIN | EPOLLET`. When new TCP SYN packets complete the three-way handshake, `epoll_wait` triggers once. The server MUST execute an exhaustive loop calling `accept4` until `errno == EAGAIN || errno == EWOULDBLOCK`.
- **Client Sockets (`client_fd`)**: Registered with `EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT`. The server must read available bytes in a loop using `recv(..., MSG_DONTWAIT)` until receiving `EAGAIN` or `EWOULDBLOCK`.

### Thread-Safe Event Demuxing with `EPOLLONESHOT`
In a multithreaded architecture, if multiple worker threads interact with socket events, edge transitions could cause another thread to wake up for the same file descriptor while the first thread is still processing data.
- By using `EPOLLONESHOT`, the kernel disarms event notifications for that socket after the first event fires.
- No other thread can receive events for that socket until the active thread explicitly re-arms the descriptor using `epoll_ctl(..., EPOLL_CTL_MOD, ...)`.
- This eliminates data races on the socket descriptor without requiring coarse global locks.

### Low-Latency TCP Socket Configuration
Every accepted socket is configured immediately with performance options:
- `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`: Sets non-blocking and close-on-exec atomically in kernel space, saving two `fcntl` system calls per connection.
- `TCP_NODELAY`: Disables Nagle's algorithm (RFC 896), preventing the kernel from buffering small packets (e.g. HTTP headers) to wait for 200ms ACK timers. This drastically reduces p99 latency in request-response protocols.
- `SO_REUSEADDR` & `SO_REUSEPORT`: Enables instant rebinding without port conflicts during service restarts, and distributes incoming SYN queues across socket listeners.
- `SO_SNDBUF` & `SO_RCVBUF`: Tuned to 256KB to maximize TCP window scaling over local and high-bandwidth interconnects.

### Instantaneous Graceful Shutdown with Linux `eventfd`
Traditional epoll event loops either poll with short timeouts (burning CPU cycles) or sleep with long timeouts (introducing shutdown latency).
- This server creates an `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)` registered with epoll during initialization.
- When `Server::stop()` is invoked, it writes an 8-byte `uint64_t` value to the `wakeup_fd_`.
- `epoll_wait` breaks immediately with zero latency, allowing the server to drain connections and shut down in milliseconds.

---

## 3. Strict Concurrency Model & Deadlock Prevention

### The Lock Inversion Problem & Resolution
A common concurrency trap in network servers is lock inversion between the global connection manager and individual socket connections:

```
[DEADLOCK SCENARIO IN ORIGINAL DESIGN]
Thread 1 (I/O event):    Locks conn->mutex_        --> Attempts to lock connections_mutex_
Thread 2 (Timeout/Close): Locks connections_mutex_  --> Attempts to lock conn->mutex_
                                *** DEADLOCK ***
```

### Strict Two-Level Lock Hierarchy
We instituted an invariant lock ordering:

```
+-------------------------------------------------------------+
| Hierarchy Level 1: connections_mutex_                       |
|   Protects: std::unordered_map<int, shared_ptr<Connection>> |
+-------------------------------------------------------------+
                              |
                              v  (Must unlock Level 1 before Level 2,
                              |   or only acquire Level 2 after Level 1)
+-------------------------------------------------------------+
| Hierarchy Level 2: Connection::mutex_                       |
|   Protects: buffer, state, offset, timestamps               |
+-------------------------------------------------------------+
```

**Invariant Rule**: Code holding `conn->mutex_` is **strictly forbidden** from acquiring `connections_mutex_`.
- To look up a connection: Lock `connections_mutex_`, copy the `std::shared_ptr<Connection>`, and unlock `connections_mutex_`.
- To mutate connection state: Lock `conn->mutex_`. Because the connection is managed by `std::shared_ptr`, it is guaranteed to remain valid in memory even if another thread closes the socket and erases it from the map.
- To close a connection:
  1. Acquire `connections_mutex_`, find the iterator, extract the `shared_ptr`, and erase from the map.
  2. Release `connections_mutex_`.
  3. Acquire `conn->mutex_` to set `closed = true`.
  4. Release `conn->mutex_`.
  5. Remove fd from epoll and call `close(fd)`.

---

## 4. Connection State Machine

Each TCP connection transitions through a deterministic state machine:

```
              accept4()
                  |
                  v
            [ CONNECTED ]
                  |
                  | EPOLLIN (edge-triggered)
                  v
             [ READING ] <-----+
                  |            | Partial read
                  | (EAGAIN)   | (Wait for more data)
                  v            |
        Is Request Complete? --+
                  |
                  | Yes (EPOLLONESHOT disarmed)
                  v
            [ PROCESSING ]  (Handled by ThreadPool Worker)
                  |
                  v
             [ WRITING ] <-----+
                  |            | Partial write
                  |            | (EAGAIN -> Arm EPOLLOUT)
                  v            |
        All Bytes Transmitted? +
                  |
                  v
             Keep-Alive?
            /           \
     Yes   /             \  No
          v               v
    [ READING ]      [ CLOSED ]
    (Re-arm EPOLLIN)  (close fd, free resources)
```

---

## 5. Memory Management & Cache Architecture

1. **Zero Raw Ownership**:
   - Socket connections: `std::shared_ptr<Connection>` ensures safe reclamation without use-after-free or double-close bugs.
   - Server subsystems: `std::unique_ptr` for `EpollWrapper`, `ThreadPool`, `FileHandler`, `RateLimiter`.
2. **Buffer Accumulation**:
   - Buffers pre-allocate capacities using `.reserve()` to avoid repeated reallocations on stream chunks.
   - Maximum request size enforced at 64KB to defend against buffer exhaustion (Slowloris attacks).
3. **Thread-Safe LRU File Cache**:
   - Backed by an `std::list<std::string>` (MRU at front, LRU at back) and an `std::unordered_map<std::string, pair<CacheEntry, list::iterator>>`.
   - Thread-safe eviction enforcing a memory limit (default 100MB) and Time-To-Live expiration (default 300s).
   - O(1) retrieval, insertion, and eviction.
