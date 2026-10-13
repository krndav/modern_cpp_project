# Linux TCP Server in C++17 — Full Documentation

A small Linux-only HTTP-like server built directly on POSIX sockets and `epoll`.
It is intended as a systems-programming learning project, not as a production web server.

## 1. What it does

- Listens on TCP port **8080** on all IPv4 interfaces (`0.0.0.0`).
- Uses nonblocking sockets and `epoll` to monitor many client connections in one event loop.
- Implements three simple routes:
  - `GET /` → `200 OK`
  - `GET /health` → `200 OK`
  - Any other GET path → `404 Not Found`
- Returns `405 Method Not Allowed` for methods other than GET.
- Returns `413 Content Too Large` when the request exceeds 8 KiB.
- Handles partial writes and retries interrupted system calls.
- Closes each connection after sending one response.
- Stops cleanly on Ctrl+C (`SIGINT`) or `SIGTERM`.

## 2. Requirements

- Linux kernel with `epoll` and `accept4` support.
- GNU Make.
- A C++ compiler with C++17 support (`g++` recommended).
- `curl` for testing.

**Important:** `epoll` is Linux-specific. This project will not compile as-is on macOS. Build and run it inside your Ubuntu Multipass VM or another Linux environment.

## 3. Project layout

```text
linux_tcp_server_cpp17/
├── main.cpp       # TCP server and event loop
├── Makefile       # Build, run, and clean targets
└── README.md      # This documentation
```

## 4. Build and run

From the project directory in Linux:

```bash
make
./tcp_server
```

Or build and run in one command:

```bash
make run
```

Expected startup output:

```text
Listening on 0.0.0.0:8080
Try: curl -i http://127.0.0.1:8080/
```

Stop the server with **Ctrl+C**.

If port 8080 is already occupied, the server prints a `bind failed` message. Stop the other process or change `PORT` near the top of `main.cpp`, then rebuild.

## 5. Test the routes

Keep the server running in one terminal. Open another terminal in the same Linux VM.

### Home route

```bash
curl -i http://127.0.0.1:8080/
```

Expected status and body:

```text
HTTP/1.1 200 OK
...
Hello from the Linux TCP server!
```

### Health route

```bash
curl -i http://127.0.0.1:8080/health
```

Expected body:

```text
OK
```

### Unknown route

```bash
curl -i http://127.0.0.1:8080/missing
```

Expected status: `404 Not Found`.

### Unsupported method

```bash
curl -i -X POST http://127.0.0.1:8080/
```

Expected status: `405 Method Not Allowed`.

### Send several requests concurrently

```bash
for i in $(seq 1 20); do
    curl -s http://127.0.0.1:8080/ &
done
wait
```

You should see the response body from each request. This is a basic concurrency test: the event loop can make progress on multiple socket connections without dedicating a thread to each one.

## 6. Access it from the Mac host

If the server runs inside a Multipass VM, first find the VM's address on the Mac:

```bash
multipass list
multipass info primary
```

Use the IPv4 address shown by `multipass info`:

```bash
curl -i http://<VM-IP>:8080/
```

For example, replace `<VM-IP>` with the current address of your VM. If host-to-guest networking is unavailable, run `curl` inside the VM first. A firewall may also block inbound connections.

## 7. How the server works

### Step 1 — Create a listening socket

```cpp
socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
```

- `AF_INET`: IPv4.
- `SOCK_STREAM`: TCP byte stream.
- `SOCK_CLOEXEC`: close the descriptor automatically if the process calls `exec`.

The server enables `SO_REUSEADDR`, makes the socket nonblocking, binds it to port 8080, and calls `listen()`.

### Step 2 — Create the epoll instance

```cpp
epoll_create1(EPOLL_CLOEXEC);
```

`epoll` lets Linux notify the program when a monitored descriptor is ready for an operation, such as accepting a connection, reading data, or writing data.

### Step 3 — Register the listening socket

```cpp
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);
```

The server registers the listening socket for `EPOLLIN`. When a client connection is waiting, the event loop wakes up.

### Step 4 — Accept clients

```cpp
accept4(server_fd, ..., SOCK_NONBLOCK | SOCK_CLOEXEC);
```

The server accepts all pending connections until `accept4()` returns `EAGAIN` or `EWOULDBLOCK`. Each accepted socket is nonblocking and is added to `epoll`.

### Step 5 — Read the request

`recv()` reads bytes into a buffer. TCP does not preserve application message boundaries: a request may arrive in multiple reads. The server appends the bytes to that client's request string and waits for the HTTP header terminator, `\r\n\r\n`.

The implementation caps the request buffer at 8 KiB.

### Step 6 — Prepare the response

The server reads the request line and chooses a route. It constructs a minimal HTTP response with a status line, `Content-Type`, `Content-Length`, and `Connection: close`.

This is intentionally a small educational parser, not a complete HTTP implementation.

### Step 7 — Write the response

`send()` may write fewer bytes than requested. The `bytes_sent` member tracks progress. If the socket would block, the server waits for another `EPOLLOUT` event and resumes from the correct offset.

`MSG_NOSIGNAL` prevents a write to a disconnected peer from raising `SIGPIPE`.

### Step 8 — Close the connection

After the response is fully sent, the server removes the descriptor from `epoll`, closes it, and erases its client state.

## 8. Key system calls and concepts

| API | Purpose |
|---|---|
| `socket()` | Create a TCP socket |
| `setsockopt()` | Configure socket options |
| `fcntl()` | Set a descriptor to nonblocking mode |
| `bind()` | Assign the local IP address and port |
| `listen()` | Mark the socket as passive/listening |
| `accept4()` | Accept a client connection |
| `epoll_create1()` | Create an epoll instance |
| `epoll_ctl()` | Add, modify, or remove monitored descriptors |
| `epoll_wait()` | Wait for ready descriptors |
| `recv()` | Receive bytes from a client |
| `send()` | Send response bytes |
| `close()` | Release a descriptor |

### `EAGAIN` and `EWOULDBLOCK`

On a nonblocking socket, these errors usually mean that no more data can be read or written right now. They are not fatal: wait for the next readiness event and try again.

### Why nonblocking sockets?

A blocking `recv()` could stall the entire event loop while one client sends data slowly. Nonblocking I/O lets the server return to `epoll_wait()` and make progress on other clients.

### Why `unordered_map<int, Client>`?

The map stores per-connection state, indexed by file descriptor. Each client has its request buffer, response buffer, current write offset, and response state.

## 9. Limitations and security notes

This project is for learning and experimentation. It is **not production-ready**.

- No TLS/HTTPS.
- Minimal request-line parsing; it does not implement the complete HTTP specification.
- One request per connection; persistent keep-alive connections are not supported.
- Does not implement request bodies, chunked transfer encoding, virtual hosts, or routing frameworks.
- No authentication, rate limiting, access logging, or configurable timeouts.
- The 8 KiB request limit is a basic guard, not a complete denial-of-service defense.
- It binds to all IPv4 interfaces. Use a firewall and avoid exposing it to untrusted networks.

## 10. Troubleshooting

### `bind failed: Address already in use`

Another process is using port 8080. Find it with:

```bash
sudo ss -ltnp 'sport = :8080'
```

Stop that process or change `PORT` in `main.cpp`.

### `epoll` or `accept4` not found

You are probably compiling on macOS or another non-Linux platform. Build inside Ubuntu/Linux instead.

### `curl: Connection refused`

Check that the server is running, that you are using the correct VM IP, and that port 8080 is reachable.

### `make: command not found`

On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential
```

### Clean the build

```bash
make clean
```

## 11. Suggested extensions

A sensible progression is:

1. Parse headers more carefully and validate the HTTP version.
2. Add a configurable port and command-line arguments.
3. Add structured logging and per-client timeouts.
4. Add static-file serving with path traversal protection.
5. Add a thread pool for CPU-heavy work while keeping socket I/O in the event loop.
6. Add tests for malformed requests and partial reads/writes.
7. Learn TLS by placing a reverse proxy in front of the server, rather than inventing a custom TLS layer.

## 12. Compile manually

If you do not want to use Make:

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o tcp_server
```

Then:

```bash
./tcp_server
```

---

**Learning objective:** understand how TCP sockets, nonblocking I/O, `epoll`, request buffering, partial writes, and descriptor lifecycle fit together in a small Linux networking program.
