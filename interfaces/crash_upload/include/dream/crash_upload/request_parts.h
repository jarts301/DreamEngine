#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::crash_upload {

// Pieces the crash_upload modules build their requests from (ADR 0043).

// Text, quoted for JSON.
[[nodiscard]] inline std::string json_string(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            quoted += "\\\"";
            break;
        case '\\':
            quoted += "\\\\";
            break;
        case '\n':
            quoted += "\\n";
            break;
        case '\r':
            quoted += "\\r";
            break;
        case '\t':
            quoted += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                quoted += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                quoted += c;
            }
        }
    }
    return quoted + "\"";
}

// 32 random hexadecimal digits, as Sentry's event ids, and Backtrace's with dashes.
[[nodiscard]] inline std::string random_id() {
    std::random_device device;
    std::string id;
    for (int index = 0; index < 4; ++index) {
        id += std::format("{:08x}", device());
    }
    return id;
}

// Seconds since 1970, as both services take times.
[[nodiscard]] inline std::int64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The first line of a report, which names what happened.
[[nodiscard]] inline std::string_view first_line(std::string_view text) {
    return text.substr(0, text.find('\n'));
}

// A multipart/form-data body, field by field.
class MultipartBody {
public:
    MultipartBody() : m_boundary("dream-" + random_id()) {}

    void add_field(std::string_view name, std::string_view value) {
        append(std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"\r\n\r\n", m_boundary,
                           name));
        append(value);
        append("\r\n");
    }

    void add_file(std::string_view name, std::string_view file_name, std::string_view type,
                  const std::vector<std::byte>& contents) {
        append(std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
                           "Content-Type: {}\r\n\r\n",
                           m_boundary, name, file_name, type));
        m_body.insert(m_body.end(), contents.begin(), contents.end());
        append("\r\n");
    }

    // The header the body goes with.
    [[nodiscard]] std::string content_type() const {
        return "multipart/form-data; boundary=" + m_boundary;
    }

    // The body, closed.
    [[nodiscard]] std::vector<std::byte> finish() {
        append(std::format("--{}--\r\n", m_boundary));
        return std::move(m_body);
    }

private:
    void append(std::string_view text) {
        for (const char c : text) {
            m_body.push_back(static_cast<std::byte>(c));
        }
    }

    std::string m_boundary;
    std::vector<std::byte> m_body;
};

[[nodiscard]] inline std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        bytes[index] = static_cast<std::byte>(text[index]);
    }
    return bytes;
}

} // namespace dream::crash_upload
