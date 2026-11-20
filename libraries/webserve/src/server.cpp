#include "webserve/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>

namespace webserve {
namespace {

constexpr std::size_t k_max_header_bytes = 64 * 1024;
constexpr std::size_t k_max_body_bytes = 8 * 1024 * 1024;
// How long an idle kept-alive connection stays open.
constexpr int k_idle_timeout_ms = 15000;
// How often waiting threads check whether the server is stopping.
constexpr int k_poll_slice_ms = 200;

std::string_view status_text(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool send_all(int socket, std::string_view data) {
    while (!data.empty()) {
        const ssize_t sent = ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string url_decode(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '+') {
            result.push_back(' ');
        } else if (c == '%' && index + 2 < text.size() && hex_value(text[index + 1]) >= 0 &&
                   hex_value(text[index + 2]) >= 0) {
            result.push_back(
                static_cast<char>(hex_value(text[index + 1]) * 16 + hex_value(text[index + 2])));
            index += 2;
        } else {
            result.push_back(c);
        }
    }
    return result;
}

std::optional<std::string> query_value(std::string_view query, std::string_view key) {
    while (!query.empty()) {
        const std::size_t end = query.find('&');
        const std::string_view pair = query.substr(0, end);
        const std::size_t equals = pair.find('=');
        if (url_decode(pair.substr(0, equals)) == key) {
            return equals == std::string_view::npos ? std::string()
                                                    : url_decode(pair.substr(equals + 1));
        }
        if (end == std::string_view::npos) {
            break;
        }
        query.remove_prefix(end + 1);
    }
    return std::nullopt;
}

std::string html_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&#39;";
            break;
        default:
            result.push_back(c);
        }
    }
    return result;
}

std::string json_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr std::string_view digits = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xf]);
                result.push_back(digits[static_cast<unsigned char>(c) & 0xf]);
            } else {
                result.push_back(c);
            }
        }
    }
    return result;
}

Server::~Server() {
    stop();
}

std::optional<std::string> Server::start(const std::string& host, std::uint16_t port,
                                         Handler handler) {
    if (m_listener >= 0) {
        return "The server is already started";
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        return "'" + host + "' is not an IPv4 address, such as 127.0.0.1 or 0.0.0.0";
    }
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        return std::string("Could not open a socket: ") + std::strerror(errno);
    }
    const int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 64) != 0) {
        const std::string message = std::strerror(errno);
        ::close(listener);
        return "Could not listen on " + host + ":" + std::to_string(port) + ": " + message;
    }
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size);
    m_port = ntohs(bound.sin_port);
    m_listener = listener;
    m_handler = std::move(handler);
    m_stopping = false;
    m_acceptor = std::thread([this] { accept_loop(); });
    return std::nullopt;
}

void Server::stop() {
    if (m_listener < 0) {
        return;
    }
    m_stopping = true;
    if (m_acceptor.joinable()) {
        m_acceptor.join();
    }
    ::close(m_listener);
    m_listener = -1;
    std::vector<Connection> connections;
    {
        const std::lock_guard lock(m_mutex);
        for (const int socket : m_open_sockets) {
            ::shutdown(socket, SHUT_RDWR);
        }
        connections = std::move(m_connections);
        m_connections.clear();
    }
    for (Connection& connection : connections) {
        if (connection.thread.joinable()) {
            connection.thread.join();
        }
    }
}

void Server::accept_loop() {
    while (!m_stopping) {
        pollfd waiting{.fd = m_listener, .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, k_poll_slice_ms) <= 0) {
            continue;
        }
        const int socket = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (socket < 0) {
            continue;
        }
        const int yes = 1;
        ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        const std::lock_guard lock(m_mutex);
        // Threads of connections that have closed are joined here, so they don't pile up.
        std::erase_if(m_connections, [](Connection& connection) {
            if (!connection.finished->load()) {
                return false;
            }
            connection.thread.join();
            return true;
        });
        m_open_sockets.push_back(socket);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        m_connections.push_back(Connection{
            std::thread([this, socket, finished] {
                serve(socket);
                finished->store(true);
            }),
            finished});
    }
}

void Server::serve(int socket) {
    std::string buffer;
    std::array<char, 16384> chunk{};
    bool keep_alive = true;
    // Reads until `wanted` bytes are buffered, or fails on close, timeout or stop.
    const auto fill = [&](std::size_t wanted) {
        int idle_ms = 0;
        while (buffer.size() < wanted) {
            if (m_stopping) {
                return false;
            }
            pollfd waiting{.fd = socket, .events = POLLIN, .revents = 0};
            const int ready = ::poll(&waiting, 1, k_poll_slice_ms);
            if (ready == 0) {
                idle_ms += k_poll_slice_ms;
                if (idle_ms >= k_idle_timeout_ms) {
                    return false;
                }
                continue;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            const ssize_t received = ::recv(socket, chunk.data(), chunk.size(), 0);
            if (received <= 0) {
                return false;
            }
            buffer.append(chunk.data(), static_cast<std::size_t>(received));
            idle_ms = 0;
        }
        return true;
    };

    while (keep_alive && !m_stopping) {
        std::size_t header_end = std::string::npos;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (buffer.size() > k_max_header_bytes || !fill(buffer.size() + 1)) {
                header_end = std::string::npos;
                break;
            }
        }
        if (header_end == std::string::npos) {
            break;
        }

        Request request;
        const std::string_view head(buffer.data(), header_end);
        const std::size_t line_end = head.find("\r\n");
        const std::string_view request_line = head.substr(0, line_end);
        const std::size_t first_space = request_line.find(' ');
        const std::size_t second_space = request_line.find(' ', first_space + 1);
        if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
            send_all(socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            break;
        }
        request.method = std::string(request_line.substr(0, first_space));
        const std::string_view target =
            request_line.substr(first_space + 1, second_space - first_space - 1);
        const std::size_t question = target.find('?');
        request.path = url_decode(target.substr(0, question));
        if (question != std::string_view::npos) {
            request.query = std::string(target.substr(question + 1));
        }
        const bool http10 = request_line.substr(second_space + 1) == "HTTP/1.0";
        keep_alive = !http10;

        std::size_t content_length = 0;
        std::size_t position = line_end == std::string_view::npos ? head.size() : line_end + 2;
        while (position < head.size()) {
            std::size_t end = head.find("\r\n", position);
            if (end == std::string_view::npos) {
                end = head.size();
            }
            const std::string_view line = head.substr(position, end - position);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string name = lower(line.substr(0, colon));
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') {
                    value.remove_prefix(1);
                }
                if (name == "content-length") {
                    content_length = static_cast<std::size_t>(std::strtoull(std::string(value).c_str(), nullptr, 10));
                } else if (name == "connection") {
                    const std::string connection = lower(value);
                    if (connection == "close") {
                        keep_alive = false;
                    } else if (connection == "keep-alive") {
                        keep_alive = true;
                    }
                }
            }
            position = end + 2;
        }
        if (content_length > k_max_body_bytes) {
            send_all(socket, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            break;
        }
        const std::size_t body_start = header_end + 4;
        if (!fill(body_start + content_length)) {
            break;
        }
        request.body = buffer.substr(body_start, content_length);
        buffer.erase(0, body_start + content_length);

        Response response = m_handler ? m_handler(request) : Response{.status = 404};
        std::string reply = "HTTP/1.1 " + std::to_string(response.status) + " " +
                            std::string(status_text(response.status)) + "\r\n";
        reply += "Content-Type: " + response.content_type + "\r\n";
        reply += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        reply += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        bool cache_set = false;
        for (const auto& [name, value] : response.headers) {
            reply += name + ": " + value + "\r\n";
            cache_set = cache_set || lower(name) == "cache-control";
        }
        if (!cache_set) {
            reply += "Cache-Control: no-store\r\n";
        }
        reply += "\r\n";
        if (!send_all(socket, reply) || !send_all(socket, response.body)) {
            break;
        }
    }

    const std::lock_guard lock(m_mutex);
    std::erase(m_open_sockets, socket);
    ::close(socket);
}

} // namespace webserve
