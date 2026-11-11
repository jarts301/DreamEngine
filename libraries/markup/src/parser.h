#pragma once

// Reading documents and style sheets.

#include "dom.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace markup::detail {

// The element tree of a document, under an unnamed root element. Unknown or misplaced tags are
// kept: the parser is forgiving, like a browser's.
[[nodiscard]] std::unique_ptr<Element> parse_markup(std::string_view text);

struct Declaration {
    std::string property;
    std::string value;
};

// One part of a selector, such as "button.big:hover".
struct Compound {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::vector<std::string> pseudo_classes;
    // [name] or [name=value]; an empty value only asks that the attribute exist.
    std::vector<std::pair<std::string, std::string>> attributes;
};

struct Selector {
    // Left to right. combinators[i] joins compounds[i] and compounds[i + 1]: ' ' or '>'.
    std::vector<Compound> compounds;
    std::vector<char> combinators;
    int specificity = 0;
};

struct Rule {
    std::vector<Selector> selectors;
    std::vector<Declaration> declarations;
};

struct StyleSheet {
    std::vector<Rule> rules;
};

// Adds the rules of a CSS text. Rules it can't read, and at-rules, are skipped.
void parse_css(std::string_view text, StyleSheet& sheet);

// The declarations of a style attribute or a rule's body.
[[nodiscard]] std::vector<Declaration> parse_declarations(std::string_view text);

// Lower case, for the ASCII letters that tags and property names use.
[[nodiscard]] std::string lower(std::string_view text);

} // namespace markup::detail
