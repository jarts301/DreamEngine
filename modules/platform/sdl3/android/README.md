# SDL3 on Android

`java/org/libsdl/app/` is copied unmodified from SDL 3.4.18 (`android-project/app/src/main/java`),
under the license in `LICENSE-SDL.txt`. SDL refuses to start if these files and the native library
come from different versions, so copy them again whenever the SDL version in vcpkg changes.

`java/org/dreamengine/app/DreamActivity.java` is the engine's own entry activity.
