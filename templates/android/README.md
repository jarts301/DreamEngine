# Android APK template

The Gradle project that packages any game into an APK. It compiles no C++: CMake builds the game
first, then runs this project through the game's `<name>_apk` target. The target passes the game's
application ID, display name, native library, assets and any Java code from its backends.

See the Android section of the [main README](../../README.md).
