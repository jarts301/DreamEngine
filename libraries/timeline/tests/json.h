#pragma once

// A small, strict JSON reader, so tests can check that traces are valid JSON and look inside them.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;
    // An object's keys and values, in the same order.
    std::vector<std::string> keys;

    // The member named `key`, or null if this isn't an object or has no such member.
    [[nodiscard]] const Value* find(std::string_view key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &items[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::string_view text(std::string_view key) const {
        const Value* const member = find(key);
        return member != nullptr && member->type == Type::String ? std::string_view(member->string)
                                                                 : std::string_view{};
    }

    [[nodiscard]] double number_of(std::string_view key) const {
        const Value* const member = find(key);
        return member != nullptr && member->type == Type::Number ? member->number : -1.0;
    }
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::optional<Value> document() {
        std::optional<Value> value = parse_value(0);
        skip_space();
        if (!value.has_value() || m_position != m_text.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int k_max_depth = 64;

    void skip_space() {
        while (m_position < m_text.size() &&
               (m_text[m_position] == ' ' || m_text[m_position] == '\n' ||
                m_text[m_position] == '\r' || m_text[m_position] == '\t')) {
            ++m_position;
        }
    }

    bool take(char c) {
        skip_space();
        if (m_position < m_text.size() && m_text[m_position] == c) {
            ++m_position;
            return true;
        }
        return false;
    }

    bool take_word(std::string_view word) {
        if (m_text.substr(m_position, word.size()) == word) {
            m_position += word.size();
            return true;
        }
        return false;
    }

    std::optional<Value> parse_value(int depth) {
        if (depth > k_max_depth) {
            return std::nullopt;
        }
        skip_space();
        if (m_position >= m_text.size()) {
            return std::nullopt;
        }
        Value value;
        const char c = m_text[m_position];
        if (c == '{') {
            ++m_position;
            value.type = Value::Type::Object;
            if (take('}')) {
                return value;
            }
            do {
                skip_space();
                std::optional<std::string> key = parse_string();
                if (!key.has_value() || !take(':')) {
                    return std::nullopt;
                }
                std::optional<Value> member = parse_value(depth + 1);
                if (!member.has_value()) {
                    return std::nullopt;
                }
                value.keys.push_back(std::move(*key));
                value.items.push_back(std::move(*member));
            } while (take(','));
            return take('}') ? std::optional<Value>(std::move(value)) : std::nullopt;
        }
        if (c == '[') {
            ++m_position;
            value.type = Value::Type::Array;
            if (take(']')) {
                return value;
            }
            do {
                std::optional<Value> item = parse_value(depth + 1);
                if (!item.has_value()) {
                    return std::nullopt;
                }
                value.items.push_back(std::move(*item));
            } while (take(','));
            return take(']') ? std::optional<Value>(std::move(value)) : std::nullopt;
        }
        if (c == '"') {
            std::optional<std::string> text = parse_string();
            if (!text.has_value()) {
                return std::nullopt;
            }
            value.type = Value::Type::String;
            value.string = std::move(*text);
            return value;
        }
        if (take_word("true") || take_word("false")) {
            value.type = Value::Type::Bool;
            value.boolean = m_text[m_position - 1] == 'e' && m_text[m_position - 2] == 'u';
            return value;
        }
        if (take_word("null")) {
            return value;
        }
        return parse_number();
    }

    std::optional<Value> parse_number() {
        // JSON's grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
        const std::size_t start = m_position;
        const auto digits = [this] {
            const std::size_t first = m_position;
            while (m_position < m_text.size() && m_text[m_position] >= '0' &&
                   m_text[m_position] <= '9') {
                ++m_position;
            }
            return m_position - first;
        };
        if (m_position < m_text.size() && m_text[m_position] == '-') {
            ++m_position;
        }
        const std::size_t integer_start = m_position;
        const std::size_t integer_digits = digits();
        if (integer_digits == 0 || (integer_digits > 1 && m_text[integer_start] == '0')) {
            return std::nullopt;
        }
        if (m_position < m_text.size() && m_text[m_position] == '.') {
            ++m_position;
            if (digits() == 0) {
                return std::nullopt;
            }
        }
        const auto next_is = [this](char a, char b) {
            return m_position < m_text.size() &&
                   (m_text[m_position] == a || m_text[m_position] == b);
        };
        if (next_is('e', 'E')) {
            ++m_position;
            if (next_is('+', '-')) {
                ++m_position;
            }
            if (digits() == 0) {
                return std::nullopt;
            }
        }
        const std::string number(m_text.substr(start, m_position - start));
        Value value;
        value.type = Value::Type::Number;
        value.number = std::strtod(number.c_str(), nullptr);
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xc0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }

    std::optional<std::uint32_t> parse_hex4() {
        if (m_position + 4 > m_text.size()) {
            return std::nullopt;
        }
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = m_text[m_position++];
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code |= static_cast<std::uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<std::uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<std::uint32_t>(h - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return code;
    }

    std::optional<std::string> parse_string() {
        if (m_position >= m_text.size() || m_text[m_position] != '"') {
            return std::nullopt;
        }
        ++m_position;
        std::string out;
        while (m_position < m_text.size()) {
            const char c = m_text[m_position++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::nullopt; // Control characters must be escaped.
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_position >= m_text.size()) {
                return std::nullopt;
            }
            switch (m_text[m_position++]) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                const std::optional<std::uint32_t> code = parse_hex4();
                if (!code.has_value()) {
                    return std::nullopt;
                }
                append_utf8(out, *code);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::string_view m_text;
    std::size_t m_position = 0;
};

} // namespace detail

// Returns nothing if `text` isn't exactly one valid JSON value.
[[nodiscard]] inline std::optional<Value> parse(std::string_view text) {
    return detail::Parser(text).document();
}

} // namespace json
