package dev.sdlos.app;

// android/app/src/main/java/dev/sdlos/app/MainActivity.java
// Thin subclass of SDLActivity — nothing to override for a basic sdlos app.
// SDL3's SDLActivity handles the full lifecycle; just sub-classing is enough
// to change the package/application name that appears on the launcher.

import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    // Override getMainSharedObject() if you rename the shared library
    // in CMakeLists.txt from "main" to something else.
    // @Override
    // protected String getMainSharedObject() { return "libsdlos.so"; }
}

