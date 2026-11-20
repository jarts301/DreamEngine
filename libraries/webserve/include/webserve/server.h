#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// A small HTTP/1.1 server on POSIX sockets, for showing programs in a web browser on machines with
// no display (ADR 0039). It serves one handler, on a thread per connection, so a handler may wait,
// as long polling does. It is meant for a browser on the same machine or a trusted network: there
// is no TLS and no authentication.
namespace webserve {

struct Request {
    std::string method;
    // Without the query, percent-decoded, such as "/frame".
    std::string path;
    // What follows '?', still encoded; see query_value.
    std::string query;
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    // Extra headers, such as {"Cache-Control", "no-store"}.
    std::vector<std::pair<std::string, std::string>> headers;
};

using Handler = std::function<Response(const Request&)>;

// The value of `key` in a query such as "a=1&b=two", decoded, or nothing.
[[nodiscard]] std::optional<std::string> query_value(std::string_view query, std::string_view key);

// Decodes %XX escapes, and '+' as a space.
[[nodiscard]] std::string url_decode(std::string_view text);

// Escapes text for HTML and JSON strings.
[[nodiscard]] std::string html_escape(std::string_view text);
[[nodiscard]] std::string json_escape(std::string_view text);

class Server {
public:
    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Listens on `host`, such as "127.0.0.1" for this machine only or "0.0.0.0" for every network,
    // and `port`, where 0 picks a free one. Returns an error message on failure.
    [[nodiscard]] std::optional<std::string> start(const std::string& host, std::uint16_t port,
                                                   Handler handler);

    // Closes every connection and waits for their threads. Handlers still running should return
    // within a second or so; long polls should wait in short slices and check stopping().
    void stop();

    [[nodiscard]] bool stopping() const noexcept {
        return m_stopping.load();
    }

    // The port it listens on, once started.
    [[nodiscard]] std::uint16_t port() const noexcept {
        return m_port;
    }

private:
    void accept_loop();
    void serve(int socket);

    Handler m_handler;
    int m_listener = -1;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stopping{false};
    std::thread m_acceptor;
    std::mutex m_mutex;
    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Connection> m_connections;
    std::vector<int> m_open_sockets;
};

} // namespace webserve
