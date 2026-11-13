package org.dreamengine.app;

import org.libsdl.app.SDLActivity;

/** The Android activity for games using the SDL3 platform backend. */
public class DreamActivity extends SDLActivity {
    /** SDL is linked statically into the game, so only one library has to be loaded. */
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    /** Defined in runtime/entry/game_entry.cpp. */
    @Override
    protected String getMainFunction() {
        return "dream_android_main";
    }

    /**
     * The app's private files folder, which the game makes its working directory, since the one
     * Android starts it in can't be written. Files written to relative paths, such as profiles,
     * land there.
     */
    @Override
    protected String[] getArguments() {
        return new String[] {getFilesDir().getAbsolutePath()};
    }
}
