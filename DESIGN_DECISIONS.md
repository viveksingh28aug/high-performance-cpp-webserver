# Technical Design Decisions & Trade-Offs

This document presents the core engineering trade-offs, rationale, and design decisions made during the development and hardening of the **Multithreaded Edge-Triggered HTTP Web Server**. These topics directly reflect real-world architectural considerations in high-performance network software, silicon micro-engines, and carrier-grade routing stacks.

---

## 1. I/O Multiplexing: Edge-Triggered (`EPOLLET`) vs. Level-Triggered

### Decision
Utilize Edge-Triggered (`EPOLLET`) event notifications with non-blocking sockets for both the listening socket and active client connections.

### Analysis & Trade-Offs
- **Level-Triggered (LT)**:
  - *Mechanism*: `epoll_wait` returns as long as data remains in the kernel socket receive buffer.
  - *Pros*: Forgiving programming model; partial reads do not stall the connection.
  - *Cons*: System call overhead and redundant event delivery ("event storms") when buffer space cannot be drained in a single pass.
- **Edge-Triggered (ET)**:
  - *Mechanism*: `epoll_wait` delivers an event notification *only* on state transitions (e.g., from unreadable to readable, or unwritable to writable).
  - *Pros*: Minimizes `epoll_wait` wakeups; ideal for high-throughput batching; mirrors hardware interrupt signaling in ASIC network interfaces.
  - *Cons / Challenges*:
    1. *Starvation / Hanging*: If userland code does not read all available bytes down to `EAGAIN`/`EWOULDBLOCK`, the socket will permanently stall because no new edge notification will arrive for existing buffered bytes.
    2. *Solution*: We implemented an invariant `while (true)` loop wrapping `recv` and `send` until `EAGAIN` or `EWOULDBLOCK` is explicitly returned.

---

## 2. Event Demultiplexing: `EPOLLONESHOT` for Multithreaded Safety

### Decision
Arm client connection descriptors with `EPOLLONESHOT` combined with `EPOLLET`.

### Analysis & Trade-Offs
- **Problem**: When multiple threads drive or handle epoll events, an incoming packet edge on an active descriptor might trigger a second event while worker thread A is still parsing a previous request on the same descriptor. This introduces data races and concurrent socket access.
- **Alternative 1: Userland Atomic State Tracking**:
  - Requires maintaining an atomic lock flag per connection. If another event fires while locked, the thread must defer or re-queue the event. Adds cache-line contention and lock complexity.
- **Alternative 2: Kernel `EPOLLONESHOT` (Chosen)**:
  - The kernel automatically de-registers the descriptor from the ready list once an event is reported.
  - Guarantees that at any point in time, *at most one worker thread* is handling I/O or processing for a given socket.
  - Once processing or asynchronous transmission completes, the worker explicitly re-arms the socket using `epoll_ctl(..., EPOLL_CTL_MOD, ...)`.
  - *Cost*: One extra `epoll_ctl` syscall per request cycle, which is outweighed by total elimination of race conditions and lock-free thread isolation.

---

## 3. Concurrency Architecture: Decoupled Reactor + Thread Pool vs. Thread-per-Core

### Decision
Implement a single-reactor event dispatch loop coupled to a worker thread pool (`std::thread::hardware_concurrency()`) via a synchronized FIFO task queue.

### Analysis & Trade-Offs
- **Thread-Per-Core (Shared-Nothing / Seastar / DPDK style)**:
  - *Pros*: Zero inter-thread synchronization; descriptors partitioned by CPU core.
  - *Cons*: Load imbalance if connections are unevenly distributed; requires `SO_REUSEPORT` with kernel packet steering or hardware RSS (Receive Side Scaling).
- **Decoupled Reactor + Worker Pool (Chosen)**:
  - *Pros*: Excellent balance for general-purpose network servers; simple load distribution (any idle worker can immediately pick up completed requests); I/O reactor never stalls on compute-heavy tasks or disk accesses.
  - *Cons*: Mutex contention on the task queue under extreme concurrency (>200k req/s).
  - *Mitigation*: Workers execute the full parsing and initial non-blocking socket transmission directly, reducing context-switch handoffs back to the event loop.

---

## 4. Lock Hierarchy vs. Lock-Free Synchronization

### Decision
A deterministic two-level mutex hierarchy (`connections_mutex_` -> `Connection::mutex_`) combined with `std::shared_ptr` lifecycle tracking.

### Analysis & Trade-Offs
- **Initial Flaw Found**: The original implementation suffered from lock inversion (ABBA deadlock) between the connection map lock and connection instance locks.
- **Options Considered**:
  1. *Lock-free Map (e.g. hazard pointers or RCU)*: Very high implementation complexity and subtle memory-ordering edge cases in standard C++.
  2. *Strict Lock Hierarchy (Chosen)*:
     - Level 1: `connections_mutex_` (protects lookup and lifecycle).
     - Level 2: `Connection::mutex_` (protects individual buffer and transmission offsets).
     - Enforced rule: Never hold `Connection::mutex_` while requesting `connections_mutex_`.
     - `std::shared_ptr` ownership allows releasing `connections_mutex_` immediately after lookup, guaranteeing that the connection object remains alive even if closed by another thread.

---

## 5. Event Loop Wakeup: Linux `eventfd` vs. Self-Pipe vs. Sleep Polling

### Decision
Utilize Linux `eventfd(2)` with `EFD_NONBLOCK | EFD_CLOEXEC` to wake up the epoll reactor.

### Analysis & Trade-Offs
- **Polling with Sleep**: Burns CPU or introduces unacceptable shutdown latency (e.g., waiting 1000ms for a timeout).
- **Self-Pipe Trick (`pipe(2)`)**: Uses two file descriptors (read/write pipe) and has higher memory and inode overhead in the kernel.
- **Linux `eventfd(2)` (Chosen)**:
  - Lightweight single-descriptor kernel counter.
  - A single 8-byte write wakes up `epoll_wait` instantly.
  - Clean, modern Linux-native mechanism with minimal overhead.

---

## 6. HTTP/1.1 Engine & Protocol RFC Compliance

### Decision
Strict adherence to RFC 7230 and RFC 7231 specifications for HTTP/1.1 semantics.

### Key Implemented Rules:
1. **Mandatory Host Header (RFC 7230 §5.4)**:
   - All HTTP/1.1 requests must contain a valid `Host` header.
   - Any request lacking `Host` is immediately rejected with `400 Bad Request`.
2. **HEAD Method Semantics (RFC 7231 §4.3.2)**:
   - A HEAD response MUST contain identical status and header fields (including `Content-Length` and `Content-Type`) to an equivalent GET request.
   - The entity body MUST be omitted from the wire transmission.
   - *Fix applied*: Replaced naive `set_body("")` (which was improperly setting `Content-Length: 0`) with a `suppress_body(true)` flag that preserves header calculations while suppressing network payload bytes.
3. **Binary-Safe Payload Extraction**:
   - Replaced delimiter-based string stream parsing with direct memory slicing at `\r\n\r\n`.
   - Protects binary uploads and JSON payloads from byte corruption.
4. **Persistent Connection Management**:
   - HTTP/1.1 defaults to `Connection: keep-alive` unless `Connection: close` is specified.
   - Re-arms sockets for subsequent requests on the same TCP channel, saving TCP 3-way handshake overhead.
