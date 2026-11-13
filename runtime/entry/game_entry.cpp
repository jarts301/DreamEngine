// The program's entry point. dream_add_game compiles this directly into each game, because a linker
// would discard it from a static library on Android, where nothing refers to it by name.

#include "dream/runtime/program.h"

#if defined(__ANDROID__)

#include <filesystem>
#include <system_error>

// Found by name and called by the Android activity, which passes the app's private files folder
// after the program name. It becomes the working directory, because Android starts apps in one they
// can't write to, so that files written to relative paths, such as profiles, can be saved.
extern "C" __attribute__((visibility("default"))) int dream_android_main(int argc, char* argv[]) {
    if (argc > 1 && argv[1] != nullptr) {
        std::error_code error;
        std::filesystem::current_path(argv[1], error);
    }
    return dream::runtime::run_program();
}

#else

int main() {
    return dream::runtime::run_program();
}

#endif
