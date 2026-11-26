#pragma once

#include <span>
#include <string>
#include <string_view>

namespace dream::runtime {

// Loads the project's modules, then creates and runs the game. Returns the process exit code.
[[nodiscard]] int run_program();

// The arguments the program was started with, after its own name, such as the project folder the
// editor opens. The entry point sets them; on Android there are none.
void set_program_arguments(int argc, const char* const* argv);
[[nodiscard]] std::span<const std::string> program_arguments() noexcept;

// The game's version, such as "1.4.2" (ADR 0048), which the build gives its entry point from
// dream_add_game. Empty for a program that doesn't set it.
void set_game_version(std::string_view version);
[[nodiscard]] std::string_view game_version() noexcept;

} // namespace dream::runtime
