// Packages a game that CMake has already compiled. Run it through the <game>_apk CMake target,
// which supplies these properties.

plugins {
    id("com.android.application")
}

fun dreamProperty(name: String): String = providers.gradleProperty(name).getOrElse("")

android {
    namespace = "org.dreamengine.template"
    compileSdk = 35
    buildToolsVersion = "35.0.0"

    defaultConfig {
        applicationId = dreamProperty("dreamApplicationId")
        minSdk = 29
        targetSdk = 35
        // The game's version, from dream_add_game (ADR 0048).
        versionCode = dreamProperty("dreamVersionCode").toIntOrNull() ?: 1
        versionName = dreamProperty("dreamVersionName").ifEmpty { "1.0" }
        manifestPlaceholders["dreamDisplayName"] = dreamProperty("dreamDisplayName")
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // Gradle strips the release libraries' symbols from what ships with the NDK's tools.
    dreamProperty("dreamNdkDir").takeIf { it.isNotEmpty() }?.let { ndkPath = it }
    dreamProperty("dreamNdkVersion").takeIf { it.isNotEmpty() }?.let { ndkVersion = it }

    buildTypes {
        release {
            // Lets a release build be installed for testing. Publishing needs a real signing key.
            signingConfig = signingConfigs.getByName("debug")
            // The symbols go into the bundle, for Google Play's crash reports (ADR 0048).
            ndk {
                debugSymbolLevel = "FULL"
            }
        }
    }

    sourceSets.getByName("main") {
        // The permissions the game's modules ask for, such as INTERNET for one that uploads crash
        // reports, added to a copy of the manifest. A game whose modules ask for none gets it as
        // it is.
        val permissions = dreamProperty("dreamPermissions").split(",").filter { it.isNotEmpty() }
        if (permissions.isNotEmpty()) {
            val lines = permissions.joinToString("") {
                "    <uses-permission android:name=\"android.permission.$it\" />\n"
            }
            val manifest = file("src/main/AndroidManifest.xml").readText()
                .replaceFirst("\n    <application", "\n" + lines + "\n    <application")
            val copy = file(dreamProperty("dreamBuildDir") + "/manifest/AndroidManifest.xml")
            copy.parentFile.mkdirs()
            copy.writeText(manifest)
            this.manifest.srcFile(copy)
        }
        java.srcDirs(dreamProperty("dreamJavaDirs").split(",").filter { it.isNotEmpty() })
        jniLibs.srcDir(dreamProperty("dreamNativeDir"))
        assets.srcDirs(dreamProperty("dreamAssetDirs").split(",").filter { it.isNotEmpty() })
    }

    packaging {
        jniLibs {
            // SDL opens the game's library by its path on disk, which exists only if libraries are
            // unpacked at install time.
            useLegacyPackaging = true
            // Debug builds keep the libraries' symbols; release builds ship them stripped.
            if (dreamProperty("dreamKeepSymbols") == "true") {
                keepDebugSymbols += "**/*.so"
            }
        }
    }

    lint {
        checkReleaseBuilds = false
    }
}
