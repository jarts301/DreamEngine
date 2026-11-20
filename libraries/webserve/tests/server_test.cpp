#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "webserve/server.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <ostream> // Needed for doctest to print std::string.
#include <string>

namespace {

// Sends one request on a new connection and returns the whole reply.
std::string http_exchange(std::uint16_t port, const std::string& request) {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(socket >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    REQUIRE(::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::send(socket, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size()));
    std::string reply;
    std::array<char, 4096> chunk{};
    while (true) {
        const ssize_t received = ::recv(socket, chunk.data(), chunk.size(), 0);
        if (received <= 0) {
            break;
        }
        reply.append(chunk.data(), static_cast<std::size_t>(received));
    }
    ::close(socket);
    return reply;
}

} // namespace

TEST_CASE("query_value and url_decode read form-style queries") {
    CHECK(webserve::query_value("a=1&name=two%20words&c", "name") == "two words");
    CHECK(webserve::query_value("a=1&c", "c") == "");
    CHECK_FALSE(webserve::query_value("a=1", "b").has_value());
    CHECK(webserve::url_decode("x+y%2Fz") == "x y/z");
    CHECK(webserve::html_escape("<a href=\"x\">") == "&lt;a href=&quot;x&quot;&gt;");
    CHECK(webserve::json_escape("line\n\"q\"") == R"(line\n\"q\")");
}

TEST_CASE("The server answers GET and POST requests through its handler") {
    webserve::Server server;
    const std::optional<std::string> error =
        server.start("127.0.0.1", 0, [](const webserve::Request& request) {
            if (request.path == "/echo") {
                return webserve::Response{.body = request.method + ":" + request.body + ":" +
                                                  webserve::query_value(request.query, "q").value_or("")};
            }
            return webserve::Response{.status = 404, .body = "missing"};
        });
    REQUIRE_FALSE(error.has_value());
    REQUIRE(server.port() != 0);

    const std::string get =
        http_exchange(server.port(), "GET /echo?q=hi%21 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(get.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(get.ends_with("\r\n\r\nGET::hi!"));
    CHECK(get.find("Cache-Control: no-store") != std::string::npos);

    const std::string post = http_exchange(
        server.port(),
        "POST /echo HTTP/1.1\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(post.ends_with("POST:hello:"));

    const std::string missing =
        http_exchange(server.port(), "GET /nothing HTTP/1.0\r\n\r\n");
    CHECK(missing.starts_with("HTTP/1.1 404 Not Found"));

    server.stop();
    CHECK(server.stopping());
}

TEST_CASE("The server refuses an address it can't parse") {
    webserve::Server server;
    CHECK(server.start("localhost:80", 0, {}).has_value());
}
