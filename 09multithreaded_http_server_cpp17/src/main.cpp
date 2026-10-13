#include "work_stealing_pool.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
constexpr int DEFAULT_PORT = 8080, MAX_EVENTS = 128;
constexpr std::size_t MAX_REQUEST = 8192;
volatile std::sig_atomic_t running = 1;

struct Client {
    std::uint64_t id = 0;
    std::string request, response;
    std::size_t sent = 0;
    bool processing = false, responding = false, peer_closed = false;
};
struct Completion { int fd; std::uint64_t id; std::string response; };

class CompletionQueue {
public:
    explicit CompletionQueue(int event_fd) : event_fd_(event_fd) {}
    void push(Completion c) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(c));
        }
        const std::uint64_t one = 1;
        while (true) {
            ssize_t n = ::write(event_fd_, &one, sizeof(one));
            if (n == static_cast<ssize_t>(sizeof(one))) return;
            if (n == -1 && errno == EINTR) continue;
            return; // EAGAIN means eventfd is already readable.
        }
    }
    std::vector<Completion> takeAll() {
        std::vector<Completion> out;
        std::lock_guard<std::mutex> lock(mutex_);
        out.reserve(queue_.size());
        while (!queue_.empty()) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return out;
    }
private:
    int event_fd_;
    std::mutex mutex_;
    std::deque<Completion> queue_;
};

void onSignal(int) { running = 0; }
bool parseInt(const char* s, int min, int max, int& result) {
    if (!s || !*s) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max) return false;
    result = static_cast<int>(v);
    return true;
}
bool nonBlocking(int fd) {
    int f = ::fcntl(fd, F_GETFL, 0);
    return f != -1 && ::fcntl(fd, F_SETFL, f | O_NONBLOCK) != -1;
}
bool modify(int epfd, int fd, std::uint32_t flags) {
    epoll_event ev{};
    ev.events = flags;
    ev.data.fd = fd;
    return ::epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) != -1;
}
void closeClient(int epfd, int fd, std::unordered_map<int, Client>& clients) {
    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    clients.erase(fd);
}
std::string response(int status, const std::string& reason, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
           "Content-Type: text/plain; charset=utf-8\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

// Executed on worker threads; this function never accesses client sockets.
std::string processRequest(const std::string& req) {
    auto end = req.find("\r\n");
    if (end == std::string::npos)
        return response(400, "Bad Request", "Malformed request\n");
    std::string line = req.substr(0, end);
    auto s1 = line.find(' ');
    auto s2 = s1 == std::string::npos ? std::string::npos : line.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos)
        return response(400, "Bad Request", "Malformed request line\n");
    std::string method = line.substr(0, s1);
    std::string path = line.substr(s1 + 1, s2 - s1 - 1);
    std::string version = line.substr(s2 + 1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1")
        return response(400, "Bad Request", "Unsupported HTTP version\n");
    if (path.empty() || path.front() != '/')
        return response(400, "Bad Request", "Invalid request target\n");
    if (method != "GET")
        return response(405, "Method Not Allowed", "Only GET is supported\n");
    if (path == "/")
        return response(200, "OK", "Hello from the multithreaded HTTP server!\n");
    if (path == "/health") return response(200, "OK", "OK\n");
    if (path == "/info")
        return response(200, "OK",
            "C++17 + epoll + eventfd + work-stealing thread pool\n");
    return response(404, "Not Found", "Not Found\n");
}
void dispatch(int fd, Client& c, int epfd, WorkStealingPool& pool,
              CompletionQueue& completions) {
    c.processing = true;
    std::string request_copy = std::move(c.request);
    auto id = c.id;
    try {
        auto ignored = pool.submit([fd, id, request_copy, &completions] {
            std::string reply;
            try { reply = processRequest(request_copy); }
            catch (...) { reply = response(500, "Internal Server Error", "Internal Server Error\n"); }
            completions.push(Completion{fd, id, std::move(reply)});
        });
        (void)ignored;
        if (!modify(epfd, fd, EPOLLRDHUP)) {
            c.processing = false;
            c.responding = true;
            c.response = response(500, "Internal Server Error", "Socket monitoring failed\n");
            c.sent = 0;
        }
    } catch (...) {
        c.processing = false;
        c.responding = true;
        c.response = response(503, "Service Unavailable", "Worker pool unavailable\n");
        c.sent = 0;
        modify(epfd, fd, EPOLLOUT | EPOLLRDHUP);
    }
}
void drainEventFd(int fd) {
    std::uint64_t value;
    for (;;) {
        ssize_t n = ::read(fd, &value, sizeof(value));
        if (n == static_cast<ssize_t>(sizeof(value))) continue;
        if (n == -1 && errno == EINTR) continue;
        return;
    }
}
} // namespace

int main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1 && !parseInt(argv[1], 1, 65535, port)) {
        std::cerr << "Usage: " << argv[0] << " [port] [worker_count]\n";
        return 2;
    }
    unsigned hw = std::thread::hardware_concurrency();
    int workers = static_cast<int>(hw ? hw : 2);
    if (argc > 2 && !parseInt(argv[2], 1, 256, workers)) {
        std::cerr << "worker_count must be 1..256\n";
        return 2;
    }
    if (argc > 3) {
        std::cerr << "Usage: " << argv[0] << " [port] [worker_count]\n";
        return 2;
    }

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (::sigaction(SIGINT, &sa, nullptr) == -1 ||
        ::sigaction(SIGTERM, &sa, nullptr) == -1) {
        std::cerr << "sigaction: " << std::strerror(errno) << '\n';
        return 1;
    }

    WorkStealingPool pool(static_cast<std::size_t>(workers));
    int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd == -1) {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return 1;
    }
    int reuse = 1;
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
        !nonBlocking(listen_fd)) {
        std::cerr << "socket setup: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1 ||
        ::listen(listen_fd, SOMAXCONN) == -1) {
        std::cerr << "bind/listen: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return 1;
    }
    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        std::cerr << "epoll_create1: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return 1;
    }
    int event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd == -1) {
        std::cerr << "eventfd: " << std::strerror(errno) << '\n';
        ::close(epfd); ::close(listen_fd);
        return 1;
    }
    CompletionQueue completions(event_fd);
    epoll_event ev{};
    ev.events = EPOLLIN; ev.data.fd = listen_fd;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        std::cerr << "epoll_ctl listen: " << std::strerror(errno) << '\n';
        pool.shutdown(); ::close(event_fd); ::close(epfd); ::close(listen_fd);
        return 1;
    }
    ev = {}; ev.events = EPOLLIN; ev.data.fd = event_fd;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &ev) == -1) {
        std::cerr << "epoll_ctl eventfd: " << std::strerror(errno) << '\n';
        pool.shutdown(); ::close(event_fd); ::close(epfd); ::close(listen_fd);
        return 1;
    }

    std::unordered_map<int, Client> clients;
    std::uint64_t next_id = 1;
    epoll_event events[MAX_EVENTS];
    std::cout << "Listening on 0.0.0.0:" << port << " with " << pool.threadCount()
              << " worker threads\nRoutes: GET /, /health, /info\n";

    while (running) {
        int n = ::epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait: " << std::strerror(errno) << '\n';
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            std::uint32_t flags = events[i].events;

            if (fd == event_fd) {
                drainEventFd(event_fd);
                for (auto& done : completions.takeAll()) {
                    auto it = clients.find(done.fd);
                    if (it == clients.end() || it->second.id != done.id ||
                        !it->second.processing) continue;
                    it->second.processing = false;
                    it->second.responding = true;
                    it->second.response = std::move(done.response);
                    it->second.sent = 0;
                    if (!modify(epfd, done.fd, EPOLLOUT | EPOLLRDHUP))
                        closeClient(epfd, done.fd, clients);
                }
                continue;
            }

            if (fd == listen_fd) {
                for (;;) {
                    sockaddr_in peer{};
                    socklen_t peer_len = sizeof(peer);
                    int cfd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer),
                                        &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd == -1) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::cerr << "accept4: " << std::strerror(errno) << '\n';
                        break;
                    }
                    epoll_event ce{}; ce.events = EPOLLIN | EPOLLRDHUP; ce.data.fd = cfd;
                    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ce) == -1) {
                        ::close(cfd); continue;
                    }
                    Client c; c.id = next_id++;
                    clients.emplace(cfd, std::move(c));
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) continue;
            if (flags & (EPOLLERR | EPOLLHUP)) {
                closeClient(epfd, fd, clients);
                continue;
            }

            bool close_now = false;
            if ((flags & EPOLLIN) && !it->second.processing && !it->second.responding) {
                Client& c = it->second;
                char buf[2048];
                for (;;) {
                    ssize_t got = ::recv(fd, buf, sizeof(buf), 0);
                    if (got > 0) {
                        c.request.append(buf, static_cast<std::size_t>(got));
                        if (c.request.size() > MAX_REQUEST) {
                            c.responding = true;
                            c.response = response(413, "Content Too Large", "Request too large\n");
                            c.sent = 0;
                            break;
                        }
                        if (c.request.find("\r\n\r\n") != std::string::npos) {
                            dispatch(fd, c, epfd, pool, completions);
                            break;
                        }
                        continue;
                    }
                    if (got == 0) { c.peer_closed = true; break; }
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    close_now = true; break;
                }
                if (!close_now && c.peer_closed && !c.processing && !c.responding)
                    close_now = true;
                if (!close_now && c.responding &&
                    !modify(epfd, fd, EPOLLOUT | EPOLLRDHUP)) close_now = true;
            }
            if (close_now) {
                closeClient(epfd, fd, clients);
                continue;
            }

            it = clients.find(fd);
            if (it == clients.end()) continue;
            if (flags & EPOLLRDHUP) {
                it->second.peer_closed = true;
                if (!it->second.processing && !it->second.responding) {
                    closeClient(epfd, fd, clients);
                    continue;
                }
            }

            if ((flags & EPOLLOUT) && it->second.responding) {
                Client& c = it->second;
                bool failed = false;
                while (c.sent < c.response.size()) {
                    ssize_t wrote = ::send(fd, c.response.data() + c.sent,
                                           c.response.size() - c.sent, MSG_NOSIGNAL);
                    if (wrote > 0) { c.sent += static_cast<std::size_t>(wrote); continue; }
                    if (wrote == -1 && errno == EINTR) continue;
                    if (wrote == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    failed = true; break;
                }
                if (failed || c.sent == c.response.size())
                    closeClient(epfd, fd, clients);
            }
        }
    }

    // Workers may still publish completions; keep eventfd/queue alive until join.
    pool.shutdown();
    for (const auto& item : clients) ::close(item.first);
    clients.clear();
    ::close(event_fd); ::close(epfd); ::close(listen_fd);
    std::cout << "\nServer stopped.\n";
    return 0;
}
