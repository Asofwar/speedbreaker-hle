// SpeedBreaker. GPL-3.0-or-later (see COPYING).
package dev.asofwar.speedbreaker;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

// The game's activity: SDL's own (org.libsdl.app.SDLActivity, from the SDL release
// scripts/android/build_deps.sh builds), which loads libSDL3.so and libmain.so and
// runs the game's main() on its own thread.
public class SpeedBreakerActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // env.txt in the app's folder (Android/data/<package>/files: adb push reaches
        // it): KEY=VALUE lines, # for comments, set as the process's environment
        // before libmain.so loads, so the game's switches read when it loads see
        // them too (an app gets no environment of its own to launch with).
        // Where the native libraries and a private cache are, for a Vulkan driver
        // packaged with the app (NFSMW_VK_DRIVER=turnip: libadrenotools loads it
        // from there, video/presenter.cpp).
        try {
            Os.setenv("SB_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("SB_CODE_CACHE_DIR", getCodeCacheDir().getPath(), true);
        } catch (ErrnoException e) {
            Log.w("SpeedBreaker", "setenv: " + e);
        }
        File env = new File(getExternalFilesDir(null), "env.txt");
        if (env.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(env))) {
                for (String line; (line = r.readLine()) != null; ) {
                    line = line.trim();
                    int eq = line.indexOf('=');
                    if (line.isEmpty() || line.startsWith("#") || eq <= 0)
                        continue;
                    Os.setenv(line.substring(0, eq), line.substring(eq + 1), true);
                }
            } catch (IOException | ErrnoException e) {
                Log.w("SpeedBreaker", "env.txt: " + e);
            }
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
