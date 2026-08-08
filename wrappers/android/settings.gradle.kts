// Single-module Gradle project: this directory (wrappers/android/) IS the
// `printerdriver` library module. There is no separate nested module folder --
// build.gradle.kts at this same level applies com.android.library directly, and
// CMakeLists.txt (also at this level) reaches the SDK's C++ sources with the
// repo-relative paths "../../core" and "../../capi" (two levels up from
// wrappers/android/ to the repository root). Keeping this flat was a deliberate
// choice: a nested "printerdriver/" module folder would push those relative paths
// out to "../../../core" instead. See README.md "Repository layout".

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "printerdriver"
