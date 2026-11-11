#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace markup::detail {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == ':' ||
           c == '.' || static_cast<unsigned char>(c) >= 0x80;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

void append_utf8(std::string& out, std::uint32_t code) {
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

// Replaces &amp; and the other entities, named or numbered.
std::string decode_entities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const std::size_t end = text.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out += '&';
            continue;
        }
        const std::string_view name = text.substr(i + 1, end - i - 1);
        if (name == "amp") {
            out += '&';
        } else if (name == "lt") {
            out += '<';
        } else if (name == "gt") {
            out += '>';
        } else if (name == "quot") {
            out += '"';
        } else if (name == "apos") {
            out += '\'';
        } else if (name == "nbsp") {
            append_utf8(out, 0xa0);
        } else if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const std::string digits(name.substr(hex ? 2 : 1));
            append_utf8(out, static_cast<std::uint32_t>(
                                 std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10)));
        } else {
            out += '&';
            continue;
        }
        i = end;
    }
    return out;
}

bool is_void(std::string_view tag) {
    return tag == "br" || tag == "img" || tag == "input" || tag == "link" || tag == "meta" ||
           tag == "hr";
}

std::unique_ptr<Element> make_text(std::string text) {
    auto element = std::make_unique<Element>();
    element->text = std::move(text);
    return element;
}

// --- CSS ----------------------------------------------------------------------------------------

std::string strip_comments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end = text.find("*/", i + 2);
            if (end == std::string_view::npos) {
                break;
            }
            i = end + 1;
            out += ' ';
        } else {
            out += text[i];
        }
    }
    return out;
}

bool parse_compound(std::string_view text, Compound& compound) {
    std::size_t i = 0;
    const auto read_name = [&] {
        const std::size_t start = i;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 ||
                                   text[i] == '-' || text[i] == '_')) {
            ++i;
        }
        return std::string(text.substr(start, i - start));
    };
    if (i < text.size() && text[i] == '*') {
        ++i;
    } else if (i < text.size() && text[i] != '#' && text[i] != '.' && text[i] != ':' &&
               text[i] != '[') {
        compound.tag = lower(read_name());
    }
    while (i < text.size()) {
        const char kind = text[i++];
        if (kind == '#') {
            compound.id = read_name();
        } else if (kind == '.') {
            compound.classes.push_back(read_name());
        } else if (kind == ':') {
            if (i < text.size() && text[i] == ':') {
                // Pseudo-elements such as ::before are not supported.
                return false;
            }
            compound.pseudo_classes.push_back(lower(read_name()));
        } else if (kind == '[') {
            const std::size_t end = text.find(']', i);
            if (end == std::string_view::npos) {
                return false;
            }
            const std::string_view inside = text.substr(i, end - i);
            const std::size_t equals = inside.find('=');
            std::string name(trim(inside.substr(0, equals)));
            std::string value;
            if (equals != std::string_view::npos) {
                std::string_view quoted = trim(inside.substr(equals + 1));
                if (quoted.size() >= 2 && (quoted.front() == '"' || quoted.front() == '\'')) {
                    quoted = quoted.substr(1, quoted.size() - 2);
                }
                value = std::string(quoted);
            }
            compound.attributes.emplace_back(lower(name), value);
            i = end + 1;
        } else {
            return false;
        }
    }
    return true;
}

bool parse_selector(std::string_view text, Selector& selector) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    char pending = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (is_space(text[i])) {
            if (pending == 0) {
                pending = ' ';
            }
            ++i;
            continue;
        }
        if (text[i] == '>' || text[i] == '+' || text[i] == '~') {
            // Sibling combinators aren't supported; they read as descendants.
            pending = text[i] == '>' ? '>' : ' ';
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && !is_space(text[end]) && text[end] != '>' && text[end] != '+' &&
               text[end] != '~') {
            ++end;
        }
        Compound compound;
        if (!parse_compound(text.substr(i, end - i), compound)) {
            return false;
        }
        if (!selector.compounds.empty()) {
            selector.combinators.push_back(pending == 0 ? ' ' : pending);
        }
        selector.compounds.push_back(std::move(compound));
        pending = 0;
        i = end;
    }
    for (const Compound& compound : selector.compounds) {
        selector.specificity += compound.id.empty() ? 0 : 100;
        selector.specificity +=
            10 * static_cast<int>(compound.classes.size() + compound.pseudo_classes.size() +
                                  compound.attributes.size());
        selector.specificity += compound.tag.empty() ? 0 : 1;
    }
    return !selector.compounds.empty();
}

} // namespace

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::unique_ptr<Element> parse_markup(std::string_view text) {
    auto root = std::make_unique<Element>();
    root->tag = "#root";
    Element* current = root.get();

    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '<') {
            const std::size_t end = std::min(text.find('<', i), text.size());
            current->add_child(make_text(decode_entities(text.substr(i, end - i))));
            i = end;
            continue;
        }
        if (text.compare(i, 4, "<!--") == 0) {
            const std::size_t end = text.find("-->", i + 4);
            i = end == std::string_view::npos ? text.size() : end + 3;
            continue;
        }
        if (text.compare(i, 2, "<!") == 0 || text.compare(i, 2, "<?") == 0) {
            const std::size_t end = text.find('>', i);
            i = end == std::string_view::npos ? text.size() : end + 1;
            continue;
        }
        if (text.compare(i, 2, "</") == 0) {
            const std::size_t end = text.find('>', i);
            const std::string name = lower(trim(
                text.substr(i + 2, (end == std::string_view::npos ? text.size() : end) - i - 2)));
            // Closes the nearest open element of that name, and any left open inside it.
            for (Element* open = current; open != root.get(); open = open->parent) {
                if (open->tag == name) {
                    current = open->parent;
                    break;
                }
            }
            i = end == std::string_view::npos ? text.size() : end + 1;
            continue;
        }

        // An opening tag.
        ++i;
        std::size_t start = i;
        while (i < text.size() && is_name_char(text[i])) {
            ++i;
        }
        auto element = std::make_unique<Element>();
        element->tag = lower(text.substr(start, i - start));
        bool self_closing = false;
        while (i < text.size() && text[i] != '>') {
            if (is_space(text[i])) {
                ++i;
                continue;
            }
            if (text[i] == '/') {
                self_closing = true;
                ++i;
                continue;
            }
            start = i;
            while (i < text.size() && is_name_char(text[i])) {
                ++i;
            }
            if (i == start) {
                ++i;
                continue;
            }
            const std::string name = lower(text.substr(start, i - start));
            std::string value;
            while (i < text.size() && is_space(text[i])) {
                ++i;
            }
            if (i < text.size() && text[i] == '=') {
                ++i;
                while (i < text.size() && is_space(text[i])) {
                    ++i;
                }
                if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                    const char quote = text[i++];
                    const std::size_t end = std::min(text.find(quote, i), text.size());
                    value = decode_entities(text.substr(i, end - i));
                    i = std::min(end + 1, text.size());
                } else {
                    start = i;
                    while (i < text.size() && !is_space(text[i]) && text[i] != '>') {
                        ++i;
                    }
                    value = decode_entities(text.substr(start, i - start));
                }
            }
            element->attributes[name] = value;
        }
        i = std::min(i + 1, text.size());

        element->id = element->attribute("id");
        const std::string classes = element->attribute("class");
        std::size_t c = 0;
        while (c < classes.size()) {
            while (c < classes.size() && is_space(classes[c])) {
                ++c;
            }
            const std::size_t end = std::min(classes.find_first_of(" \t\n\r", c), classes.size());
            if (end > c) {
                element->classes.push_back(classes.substr(c, end - c));
            }
            c = end;
        }

        const std::string tag = element->tag;
        Element* const added = current->add_child(std::move(element));
        if (tag == "style" || tag == "script") {
            // Their contents are raw text up to the closing tag.
            const std::string closing = "</" + tag;
            std::size_t end = i;
            while (end < text.size() && lower(text.substr(end, closing.size())) != closing) {
                ++end;
            }
            added->add_child(make_text(std::string(text.substr(i, end - i))));
            const std::size_t close = text.find('>', end);
            i = close == std::string_view::npos ? text.size() : close + 1;
        } else if (!self_closing && !is_void(tag)) {
            current = added;
        }
    }
    return root;
}

void parse_css(std::string_view source, StyleSheet& sheet) {
    const std::string text = strip_comments(source);
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        if (text[i] == '@') {
            // At-rules are skipped, with their block if they have one.
            const std::size_t semicolon = text.find(';', i);
            const std::size_t brace = text.find('{', i);
            if (brace != std::string::npos &&
                (semicolon == std::string::npos || brace < semicolon)) {
                int depth = 0;
                std::size_t j = brace;
                for (; j < text.size(); ++j) {
                    depth += text[j] == '{' ? 1 : (text[j] == '}' ? -1 : 0);
                    if (depth == 0) {
                        break;
                    }
                }
                i = j + 1;
            } else {
                i = semicolon == std::string::npos ? text.size() : semicolon + 1;
            }
            continue;
        }
        const std::size_t open = text.find('{', i);
        if (open == std::string::npos) {
            break;
        }
        const std::size_t close = text.find('}', open);
        const std::string_view selectors(text.data() + i, open - i);
        const std::string_view body(text.data() + open + 1,
                                    (close == std::string::npos ? text.size() : close) - open - 1);
        i = close == std::string::npos ? text.size() : close + 1;

        Rule rule;
        std::size_t start = 0;
        while (start <= selectors.size()) {
            const std::size_t comma = std::min(selectors.find(',', start), selectors.size());
            Selector selector;
            if (parse_selector(selectors.substr(start, comma - start), selector)) {
                rule.selectors.push_back(std::move(selector));
            }
            start = comma + 1;
        }
        rule.declarations = parse_declarations(body);
        if (!rule.selectors.empty() && !rule.declarations.empty()) {
            sheet.rules.push_back(std::move(rule));
        }
    }
}

std::vector<Declaration> parse_declarations(std::string_view text) {
    std::vector<Declaration> declarations;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find(';', start), text.size());
        const std::string_view item = text.substr(start, end - start);
        const std::size_t colon = item.find(':');
        if (colon != std::string_view::npos) {
            std::string value(trim(item.substr(colon + 1)));
            if (const std::size_t important = value.find("!important");
                important != std::string::npos) {
                value = std::string(trim(std::string_view(value).substr(0, important)));
            }
            const std::string property = lower(trim(item.substr(0, colon)));
            if (!property.empty() && !value.empty()) {
                declarations.push_back(Declaration{property, value});
            }
        }
        start = end + 1;
    }
    return declarations;
}

} // namespace markup::detail
