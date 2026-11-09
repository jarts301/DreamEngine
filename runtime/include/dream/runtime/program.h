#pragma once

namespace dream::runtime {

// Loads the project's modules, then creates and runs the game. Returns the process exit code.
[[nodiscard]] int run_program();

} // namespace dream::runtime
