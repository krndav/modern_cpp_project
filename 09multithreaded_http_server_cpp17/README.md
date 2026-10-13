# Multithreaded HTTP Server — C++17 + Linux epoll + eventfd

This project combines nonblocking TCP sockets, a Linux `epoll` event loop, a C++17 work-stealing thread pool, a synchronized completion queue, and `eventfd` notifications.

It is an educational systems-programming project, not a production web server.

## Requirements

- Linux (Ubuntu recommended)
- `g++` with C++17 support
- GNU Make and `curl`
- Bash for the included test scripts

**Linux only:** `epoll`, `accept4`, and `eventfd` are Linux APIs. Build inside your Ubuntu Multipass VM, not directly on macOS.

Install prerequisites on Ubuntu if needed:

```bash
sudo apt update
sudo apt install build-essential curl
```

## Project layout

```text
multithreaded_http_server_cpp17/
├── include/work_stealing_pool.hpp
├── src/main.cpp
├── src/work_stealing_pool.cpp
├── tests/smoke_test.sh
├── tests/concurrency_test.sh
├── Makefile
├── .gitignore
└── README.md
```

## Build and run

From the project root:

```bash
make
./http_server
```

The default port is `8080`; worker count defaults to `std::thread::hardware_concurrency()` (fallback: two).

You can configure both:

```bash
./http_server 8080 4
```

Usage: `./http_server [port] [worker_count]`

Port must be 1–65535 and worker count 1–256. Stop the server with Ctrl+C. The pool drains queued tasks and joins workers during shutdown.

## Routes

| Request | Result |
|---|---|
| `GET /` | `200 OK` greeting |
| `GET /health` | `200 OK`, body `OK` |
| `GET /info` | `200 OK`, server architecture |
| Other GET path | `404 Not Found` |
| Non-GET method | `405 Method Not Allowed` |
| Malformed request / unsupported HTTP version | `400 Bad Request` |
| Request larger than 8 KiB | `413 Content Too Large` |

It serves one request per connection and sends `Connection: close`.

## Test it

Keep the server running in terminal 1. In terminal 2:

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/info
curl -i http://127.0.0.1:8080/missing
curl -i -X POST http://127.0.0.1:8080/
```

Automated route tests:

```bash
make test
```

Send 100 concurrent requests:

```bash
./tests/concurrency_test.sh
```

Or choose a count:

```bash
./tests/concurrency_test.sh http://127.0.0.1:8080 500
```

The test scripts expect the server to already be running.

## Architecture

```text
Clients
   |
   v
I/O thread: accept, epoll, recv, send
   |
   | complete request string
   v
Work-stealing thread pool: parse request and build response
   |
   | Completion{fd, client_id, response}
   v
Mutex-protected completion queue
   |
   | eventfd wake-up
   v
I/O thread: validate client identity, register EPOLLOUT, send response
```

### Why one I/O owner?

Only the main I/O thread manages client sockets and the client-state map. Worker threads never call `recv`, `send`, `epoll_ctl`, or `close` on client sockets. This greatly reduces cross-thread socket races.

### Why `eventfd`?

Workers need to notify the `epoll` loop that a response is ready. A worker pushes a completion into a mutex-protected queue and writes to `eventfd`. Since `eventfd` is itself monitored by `epoll`, the I/O thread wakes up and drains the queue. The queue contains the response data; `eventfd` is the wake-up mechanism.

### Why both fd and client ID?

Linux can reuse a file descriptor after close. A slow worker may finish after its client disconnected. Every accepted connection gets a unique ID; the I/O thread applies a completion only if both the descriptor and ID still match. This prevents stale responses being delivered to a different connection.

## Source walkthrough

### `work_stealing_pool.hpp/.cpp`

Each worker owns a mutex-protected deque:
- Pop the newest task from its own queue.
- If empty, steal an older task from another worker's queue.
- `submit()` returns a `std::future`.
- `shutdown()` rejects new submissions, drains queued tasks, and joins threads.

This is a learning implementation, not a lock-free or production-grade pool.

### `main.cpp`

- `socket`, `setsockopt`, `fcntl`, `bind`, `listen`: create the nonblocking listening socket.
- `epoll_create1`, `epoll_ctl`, `epoll_wait`: monitor readiness.
- `accept4`: accept clients as nonblocking sockets.
- `recv`: accumulate bytes until the HTTP header terminator `\\r\\n\\r\\n`.
- `WorkStealingPool::submit`: dispatch request processing.
- `CompletionQueue` and `eventfd`: return worker results to the I/O thread.
- `send` and `bytes_sent`: handle partial writes.
- `MSG_NOSIGNAL`: avoid `SIGPIPE` when a client disconnects.
- `SIGINT`/`SIGTERM`: trigger shutdown.

## Concurrency and correctness details

1. **I/O thread owns client state.** Workers never access the client map.
2. **Request data is transferred to tasks.** Workers process their own request string.
3. **Completion delivery is synchronized.** The queue is guarded by a mutex.
4. **Partial writes are handled.** `bytes_sent` tracks progress and the socket is watched for `EPOLLOUT`.
5. **Stale results are ignored.** Completion includes fd and unique client ID.
6. **TCP half-close is considered.** A client may close its write side after sending the full request while still allowing the server to send a response.
7. **Shutdown order matters.** The pool is joined before `eventfd` and its completion queue are destroyed.

## Troubleshooting

### `bind/listen: Address already in use`

Find the process listening on port 8080:

```bash
sudo ss -ltnp 'sport = :8080'
```

Use a different port, e.g. `./http_server 8081 4`.

### `curl: Connection refused`

Confirm the server is running and the port matches. From your Mac, check the VM address:

```bash
multipass list
multipass info primary
```

Then try `curl http://<VM-IP>:8080/`. If host-to-guest networking is restricted, test with curl inside Ubuntu first.

### `epoll`, `eventfd`, or `accept4` is undefined

You are probably compiling on macOS or another non-Linux OS. Build inside Linux.

### `make: command not found`

```bash
sudo apt update
sudo apt install build-essential
```

### Clean and rebuild

```bash
make clean
make
```

## Limitations and security

This is not production-ready:
- No TLS/HTTPS.
- Minimal HTTP parsing; it is not a complete HTTP/1.1 implementation.
- No keep-alive or multiple requests per connection.
- No request-body or chunked-transfer support.
- No authentication, rate limiting, structured access logs, or per-client timeouts.
- No bounded task/completion queue or overload backpressure.
- Limited denial-of-service protection.
- Binds to `0.0.0.0`; firewall/network settings determine who can reach it.

Do not expose it to untrusted networks.

## Suggested extensions

1. Add read/write deadlines and idle connection timeouts.
2. Add bounded queues and overload responses.
3. Add a full HTTP parser and unit tests.
4. Add metrics and structured logs.
5. Add static-file serving with path traversal protection.
6. Benchmark against the single-threaded server.
7. Study edge-triggered `epoll` after understanding level-triggered readiness.

## Compile manually

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -Iinclude src/main.cpp src/work_stealing_pool.cpp \
    -pthread -o http_server
./http_server 8080 4
```

**Learning goal:** combine Linux networking, event-driven I/O, thread pools, work stealing, cross-thread completion delivery, and graceful resource management in one project.
