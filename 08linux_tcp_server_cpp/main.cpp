#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>

namespace {
constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 128;
constexpr std::size_t MAX_REQUEST_SIZE = 8192;

volatile std::sig_atomic_t running = 1;

struct Client {
    std::string request;
    std::string response;
    std::size_t bytes_sent = 0;
    bool responding = false;
};

void handleSignal(int) {
    running = 0;
}

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool modifyInterest(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    return ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != -1;
}

void closeClient(int epoll_fd, int fd,
                 std::unordered_map<int, Client>& clients) {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    clients.erase(fd);
}

std::string makeResponse(int status, const std::string& reason,
                         const std::string& body,
                         const std::string& content_type = "text/plain; charset=utf-8") {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
           "Content-Type: " + content_type + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

void prepareResponse(Client& client) {
    const auto line_end = client.request.find("\r\n");
    if (line_end == std::string::npos) {
        client.response = makeResponse(400, "Bad Request", "Bad Request\n");
        client.responding = true;
        return;
    }

    const std::string request_line = client.request.substr(0, line_end);
    const auto first_space = request_line.find(' ');
    const auto second_space = first_space == std::string::npos
        ? std::string::npos : request_line.find(' ', first_space + 1);

    if (first_space == std::string::npos || second_space == std::string::npos) {
        client.response = makeResponse(400, "Bad Request", "Bad Request\n");
    } else {
        const std::string method = request_line.substr(0, first_space);
        const std::string path =
            request_line.substr(first_space + 1, second_space - first_space - 1);

        if (method != "GET") {
            client.response = makeResponse(405, "Method Not Allowed",
                                           "Only GET is supported\n");
        } else if (path == "/") {
            client.response = makeResponse(200, "OK",
                                           "Hello from the Linux TCP server!\n");
        } else if (path == "/health") {
            client.response = makeResponse(200, "OK", "OK\n");
        } else {
            client.response = makeResponse(404, "Not Found", "Not Found\n");
        }
    }
    client.responding = true;
}

bool handleRead(int fd, Client& client) {
    char buffer[2048];
    bool peer_closed = false;

    while (true) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            client.request.append(buffer, static_cast<std::size_t>(count));

            if (client.request.size() > MAX_REQUEST_SIZE) {
                client.response = makeResponse(413, "Content Too Large",
                                               "Request too large\n");
                client.responding = true;
                return true;
            }

            if (client.request.find("\r\n\r\n") != std::string::npos) {
                prepareResponse(client);
                return true;
            }
            continue;
        }

        if (count == 0) {
            peer_closed = true;
            break;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        return false;
    }

    if (client.request.find("\r\n\r\n") != std::string::npos) {
        prepareResponse(client);
        return true;
    }

    // A client that closes before sending a complete request cannot be served.
    return !peer_closed;
}

bool handleWrite(int fd, Client& client) {
    while (client.bytes_sent < client.response.size()) {
        const char* data = client.response.data() + client.bytes_sent;
        const std::size_t remaining = client.response.size() - client.bytes_sent;
        const ssize_t count = ::send(fd, data, remaining, MSG_NOSIGNAL);

        if (count > 0) {
            client.bytes_sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true; // Wait for the socket to become writable again.
        }
        return false;
    }
    return false; // Response sent; close the connection.
}
} // namespace

int main() {
    struct sigaction action{};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (::sigaction(SIGINT, &action, nullptr) == -1 ||
        ::sigaction(SIGTERM, &action, nullptr) == -1) {
        std::cerr << "sigaction failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    const int server_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd == -1) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    int reuse = 1;
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
        !setNonBlocking(server_fd)) {
        std::cerr << "socket setup failed: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        std::cerr << "bind failed: " << std::strerror(errno)
                  << " (is port " << PORT << " already in use?)\n";
        ::close(server_fd);
        return 1;
    }

    if (::listen(server_fd, SOMAXCONN) == -1) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return 1;
    }

    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return 1;
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = server_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listen_event) == -1) {
        std::cerr << "epoll_ctl failed: " << std::strerror(errno) << '\n';
        ::close(epoll_fd);
        ::close(server_fd);
        return 1;
    }

    std::unordered_map<int, Client> clients;
    epoll_event events[MAX_EVENTS];

    std::cout << "Listening on 0.0.0.0:" << PORT << '\n';
    std::cout << "Try: curl -i http://127.0.0.1:" << PORT << "/\n";

    while (running) {
        const int ready = ::epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t flags = events[i].events;

            if (fd == server_fd) {
                while (true) {
                    sockaddr_in client_address{};
                    socklen_t client_length = sizeof(client_address);
                    const int client_fd = ::accept4(
                        server_fd,
                        reinterpret_cast<sockaddr*>(&client_address),
                        &client_length,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);

                    if (client_fd == -1) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "accept4 failed: " << std::strerror(errno) << '\n';
                        break;
                    }

                    epoll_event client_event{};
                    client_event.events = EPOLLIN | EPOLLRDHUP;
                    client_event.data.fd = client_fd;
                    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
                                    client_fd, &client_event) == -1) {
                        ::close(client_fd);
                        continue;
                    }
                    clients.emplace(client_fd, Client{});
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) {
                continue;
            }

            if ((flags & (EPOLLERR | EPOLLHUP)) != 0) {
                closeClient(epoll_fd, fd, clients);
                continue;
            }

            bool keep_client = true;
            if ((flags & EPOLLIN) != 0 && !it->second.responding) {
                keep_client = handleRead(fd, it->second);
                if (!keep_client) {
                    closeClient(epoll_fd, fd, clients);
                    continue;
                }
                if (it->second.responding) {
                    if (!modifyInterest(epoll_fd, fd, EPOLLOUT | EPOLLRDHUP)) {
                        closeClient(epoll_fd, fd, clients);
                        continue;
                    }
                }
            }

            // If the peer half-closed after sending a complete request, still send
            // the response. An incomplete request is closed by handleRead().
            if ((flags & EPOLLRDHUP) != 0 && !it->second.responding) {
                closeClient(epoll_fd, fd, clients);
                continue;
            }

            if ((flags & EPOLLOUT) != 0 && it->second.responding) {
                keep_client = handleWrite(fd, it->second);
                if (!keep_client) {
                    closeClient(epoll_fd, fd, clients);
                }
            }
        }
    }

    for (const auto& entry : clients) {
        ::close(entry.first);
    }
    ::close(epoll_fd);
    ::close(server_fd);
    std::cout << "\nServer stopped.\n";
    return 0;
}
