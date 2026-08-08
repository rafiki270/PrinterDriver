// PrinterDriver Android wrapper -- library module `printerdriver`.
//
// SCAFFOLD STATUS: written against Android Gradle Plugin / Kotlin / NDK versions that
// were current and mutually compatible at authoring time, but never resolved or built
// by an actual Gradle invocation -- this machine has no JVM, Gradle, or Android SDK
// (see README.md "Verification status"). Review and bump the pins below before first
// real build; nothing here has been dependency-resolved.

plugins {
    id("com.android.library") version "8.5.2"
    id("org.jetbrains.kotlin.android") version "1.9.24"
    `maven-publish`
    signing
}

android {
    namespace = "com.printerdriver"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        minSdk = 26

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-fexceptions", "-frtti")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        ndk {
            // The four ABIs the rest of the SDK (iOS/Dart siblings, docs/platforms.md)
            // treats as the supported Android set. Trim in a consuming app's own
            // build.gradle via android.defaultConfig.ndk.abiFilters, not here.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "consumer-rules.pro")
        }
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    // JVM-side unit tests only (src/test/kotlin) -- see .github-ci-example.yml and
    // README.md "Testing". No instrumented/connected test source set is wired up here;
    // src/androidTest does not exist in this scaffold.
    testOptions {
        unitTests {
            isReturnDefaultValues = true
            isIncludeAndroidResources = false
        }
    }

    publishing {
        singleVariant("release") {
            withSourcesJar()
            withJavadocJar()
        }
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.1")
    // Supplies Dispatchers.Main on Android; used by the closure-sugar surface
    // (Printer.send, see api.md §12 and README.md "Threading contract") so onProgress
    // / onResult land back on the main thread the way callback-style Android APIs
    // conventionally do.
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.1")
}

// --- Maven publishing ---------------------------------------------------------------
//
// Coordinates: com.printerdriver:printerdriver:0.1.0. Configured for GitHub Packages
// (matches this repo's remote) and structured so pointing `url` at Sonatype's OSSRH
// staging endpoint is the only change Maven Central publishing would need.

publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "com.printerdriver"
            artifactId = "printerdriver"
            version = "0.1.0"

            afterEvaluate {
                from(components["release"])
            }

            pom {
                name.set("PrinterDriver")
                description.set("Kotlin/Android wrapper for the PrinterDriver receipt-printing SDK core.")
                url.set("https://github.com/rafiki270/PrinterDriver")

                licenses {
                    license {
                        name.set("MIT License")
                        url.set("https://github.com/rafiki270/PrinterDriver/blob/main/LICENSE")
                        distribution.set("repo")
                    }
                }

                developers {
                    developer {
                        id.set("rafiki270")
                        name.set("Ondrej Rafaj")
                        email.set("ondrej.rafaj@gmail.com")
                        url.set("https://github.com/rafiki270")
                    }
                }

                scm {
                    connection.set("scm:git:git://github.com/rafiki270/PrinterDriver.git")
                    developerConnection.set("scm:git:ssh://git@github.com/rafiki270/PrinterDriver.git")
                    url.set("https://github.com/rafiki270/PrinterDriver")
                }
            }
        }
    }

    repositories {
        maven {
            name = "GitHubPackages"
            url = uri("https://maven.pkg.github.com/rafiki270/PrinterDriver")
            credentials {
                // CI supplies these as repository secrets (GITHUB_ACTOR is provided by
                // Actions automatically; GITHUB_TOKEN needs `packages: write`). Never
                // committed. See .github-ci-example.yml "publish".
                username = System.getenv("GITHUB_ACTOR")
                password = System.getenv("GITHUB_TOKEN")
            }
        }
        // TODO(Maven Central): add a second `maven { }` block pointing at Sonatype's
        // OSSRH staging repository (or use the `com.vanniktech.maven.publish` /
        // Central Portal plugin instead of this hand-rolled block) once the project
        // has a Sonatype namespace for com.printerdriver.
    }
}

// --- Signing (TODO, not yet wired up) ------------------------------------------------
//
// Maven Central requires every published artifact to carry a GPG signature; GitHub
// Packages does not. This block is intentionally inert until a signing key exists:
//   1. Generate a release key: `gpg --full-generate-key`, export the private key
//      armored: `gpg --export-secret-keys --armor <KEY_ID>`.
//   2. Store it as repo secrets ORG_GRADLE_PROJECT_signingKey (armored key text) and
//      ORG_GRADLE_PROJECT_signingPassword (its passphrase) -- Gradle's `signing`
//      plugin picks up `ORG_GRADLE_PROJECT_*` env vars as project properties
//      automatically, no extra wiring needed beyond what is already below.
//   3. Uncomment the `sign` call. `useInMemoryPgpKeys` avoids needing a local
//      ~/.gnupg keyring on the CI runner.
//
// signing {
//     val signingKey: String? by project
//     val signingPassword: String? by project
//     useInMemoryPgpKeys(signingKey, signingPassword)
//     sign(publishing.publications["release"])
// }
