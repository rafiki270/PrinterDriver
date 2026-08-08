# CMake project for libprinterdriver_jni.so -- the native half of the `printerdriver`
# Android library module. Invoked by the Android Gradle Plugin's externalNativeBuild
# (see build.gradle.kts), once per target ABI, with the NDK toolchain file already
# applied -- ANDROID_ABI/ANDROID_PLATFORM/the NDK sysroot (and therefore <jni.h>) are
# supplied by that toolchain, not by this file.
#
# UNVERIFIED: never configured or built. This machine has no Android SDK/NDK (see
# README.md "Verification status"). The equivalent host-only syntax check is
# src/main/cpp/printerdriver_jni.cpp against src/test/cpp/jni_stub.h -- see README.md
# "Syntax confidence without a JVM" -- which does NOT exercise this file at all: CMake
# itself has not been run, on Android or otherwise.

cmake_minimum_required(VERSION 3.22.1)

project(printerdriver_jni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT ANDROID)
  message(WARNING
    "printerdriver's Android CMakeLists.txt is being configured without ANDROID set. "
    "It is written for the NDK toolchain that externalNativeBuild applies "
    "(android.externalNativeBuild.cmake in build.gradle.kts) and has only ever been "
    "authored, not configured -- expect it not to find <jni.h> outside that context.")
endif()

# This module's root is wrappers/android/ (see settings.gradle.kts "single-module"
# note); the SDK's C++ sources live at the repository root, two directories up.
# Nothing under core/ or capi/ is copied or duplicated -- this target compiles the
# real sources in place.
get_filename_component(PD_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(PD_CORE_SOURCES
    ${PD_REPO_ROOT}/core/src/types.cpp
    ${PD_REPO_ROOT}/core/src/code_page.cpp
    ${PD_REPO_ROOT}/core/src/escpos_encoder.cpp
    ${PD_REPO_ROOT}/core/src/response_parser.cpp
    ${PD_REPO_ROOT}/core/src/capability_profile.cpp
    ${PD_REPO_ROOT}/core/src/job_store.cpp
    ${PD_REPO_ROOT}/core/src/transport.cpp
    ${PD_REPO_ROOT}/core/src/driver.cpp)

# The C ABI every wrapper binds (docs/api.md §9) -- pd_capi.cpp's static_asserts are
# what keep this wrapper's hand-mirrored Kotlin enums honest against the core.
set(PD_CAPI_SOURCES
    ${PD_REPO_ROOT}/capi/src/pd_capi.cpp)

# This module's own JNI glue: translates the pd.h C ABI to/from JNI calls. Does not
# touch core/ or capi/ C++ headers directly (see printerdriver_jni.cpp's own header
# comment) -- pd.h is the only contract it binds.
set(PD_JNI_SOURCES
    src/main/cpp/printerdriver_jni.cpp)

add_library(printerdriver_jni SHARED
    ${PD_CORE_SOURCES}
    ${PD_CAPI_SOURCES}
    ${PD_JNI_SOURCES})

target_include_directories(printerdriver_jni PRIVATE
    ${PD_REPO_ROOT}/core/include
    ${PD_REPO_ROOT}/capi/include
    # pd_internal.hpp, included by pd_capi.cpp only -- never included by
    # printerdriver_jni.cpp itself (that file binds pd.h only; see its header comment).
    ${PD_REPO_ROOT}/capi/src)

# POSIX threads: the core's transport/worker-thread model (pd.h "Callback threads").
# Present in Bionic's libc on Android, but find_package(Threads) still needs to run to
# populate the Threads::Threads target.
find_package(Threads REQUIRED)
target_link_libraries(printerdriver_jni PRIVATE Threads::Threads)

# __android_log_print, used by the JNI layer's own defensive logging (attach/detach
# failures, dropped callbacks) -- see printerdriver_jni.cpp.
find_library(PD_ANDROID_LOG_LIB log)
target_link_libraries(printerdriver_jni PRIVATE ${PD_ANDROID_LOG_LIB})

if(MSVC)
  target_compile_options(printerdriver_jni PRIVATE /W4)
else()
  target_compile_options(printerdriver_jni PRIVATE -Wall -Wextra -Wpedantic)
endif()
