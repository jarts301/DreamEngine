#pragma once

#include "dream/core/config.h"

#include <string>
#include <vector>

namespace dream::core {

// What a component's settings are, for tools such as the editor (ADR 0046): the reflection
// registry. A component describes its fields beside the reader that turns its settings into data,
// and tools show and change the settings through these descriptions, with the right control for
// each and its value when the setting is left out.

enum class FieldKind {
    Number,  // A float, shown with `minimum` and `maximum` when they differ.
    Integer, // A whole number.
    Flag,    // true or false.
    Text,    // A string.
    Choice,  // A string from `choices`.
    Asset,   // A string naming a file, such as "assets:/models/crate.glb".
    Vec2,    // Two floats.
    Vec3,    // Three floats.
    Color,   // Three floats from 0 to 1, red, green and blue.
};

struct FieldInfo {
    std::string name;
    FieldKind kind = FieldKind::Number;
    // What the component uses when the setting is absent. Empty text for one that must be given.
    ConfigValue fallback;
    float minimum = 0.0f;
    float maximum = 0.0f;
    std::vector<std::string> choices;
    // One line, for a tooltip.
    std::string help;
};

struct ComponentInfo {
    std::string name;
    std::vector<FieldInfo> fields;
};

} // namespace dream::core
