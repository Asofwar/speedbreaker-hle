// SpeedBreaker's Android app: the game (libmain.so) and SDL (libSDL3.so, its Java
// side org.libsdl.app) built by scripts/android/build.sh, packaged with a small
// activity of our own. No game data: the game installs from the player's own
// disc image, as on every other system.
//
// Gradle properties:
//   sbNative   the folder with jniLibs/arm64-v8a/{libmain.so,libSDL3.so} and
//              sdl-java/ (SDL's Java side, from the same SDL release):
//              scripts/android/build.sh's output, ../build/android/native by default
//   sbVersion  the version name (the game's: 0.2.0-hle.1)
plugins {
    id("com.android.application") version "9.4.0"
}

val nativeDir: String = providers.gradleProperty("sbNative").orNull
    ?: rootProject.file("../build/android/native").path
val versionLabel: String = providers.gradleProperty("sbVersion").orNull ?: "0.0.0-dev"

android {
    namespace = "dev.asofwar.speedbreaker"
    compileSdk = 37

    defaultConfig {
        applicationId = "dev.asofwar.speedbreaker"
        // Android 13: the C library's backtrace() (crash reports), memfd_create
        // and a Vulkan 1.3 driver on the phones that can run the game at all.
        minSdk = 33
        targetSdk = 37
        versionCode = 1
        versionName = versionLabel
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDir("$nativeDir/sdl-java")
            jniLibs.srcDir("$nativeDir/jniLibs")
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            // Signed with the debug key until there is a release key: a test
            // build for the player's own phone.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    packaging {
        jniLibs {
            // Extracted to the app's native library folder at install: a
            // Vulkan driver packaged with the app (Turnip) is loaded from files
            // there, by libadrenotools, with its hook libraries.
            useLegacyPackaging = true
        }
    }
}
