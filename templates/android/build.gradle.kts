plugins {
    id("com.android.application") version "8.10.1" apply false
}

// Build output goes in the CMake build folder, so this shared template stays clean.
val dreamBuildDir = providers.gradleProperty("dreamBuildDir").get()
allprojects {
    layout.buildDirectory.set(file("$dreamBuildDir/${project.name}"))
}
