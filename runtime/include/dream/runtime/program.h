#pragma once

#include <span>
#include <string>

namespace dream::runtime {

// Loads the project's modules, then creates and runs the game. Returns the process exit code.
[[nodiscard]] int run_program();

// The arguments the program was started with, after its own name, such as the project folder the
// editor opens. The entry point sets them; on Android there are none.
void set_program_arguments(int argc, const char* const* argv);
[[nodiscard]] std::span<const std::string> program_arguments() noexcept;

} // namespace dream::runtime
