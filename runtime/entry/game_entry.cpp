// The program's entry point. dream_add_game compiles this directly into each game, because a linker
// would discard it from a static library on Android, where nothing refers to it by name.

#include "dream/runtime/program.h"

#if defined(__ANDROID__)

// Found by name and called by the Android activity.
extern "C" __attribute__((visibility("default"))) int dream_android_main(int /*argc*/,
                                                                         char* /*argv*/[]) {
    return dream::runtime::run_program();
}

#else

int main() {
    return dream::runtime::run_program();
}

#endif
