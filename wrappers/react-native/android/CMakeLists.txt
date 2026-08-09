# The Android half of the packaging: one shared library holding the portable C++ core, its
# C ABI, the queue addon, the receipt DSL and cpp/PrinterDriverModule.cpp.
#
# This mirrors wrappers/android/CMakeLists.txt deliberately -- same sources, same include
# directories, same C++17 -- because it compiles THE SAME FILES. Nothing under core/ or
# capi/ is copied or duplicated for this package inside the repository; see
# scripts/stage-native-sources.mjs for why a published npm tarball is the one case where a
# copy exists, and how the two paths are told apart.
#
# UNVERIFIED: never configured, never built. This machine has no Android SDK, NDK or JVM.
# The equivalent host-only check is scripts/check_rn_cpp_syntax.sh, which compiles
# cpp/PrinterDriverModule.cpp against the real pd.h with a stub for the React Native
# headers -- and which does NOT exercise this file at all: CMake itself has not been run.
# See README.md "Verification status".

cmake_minimum_required(VERSION 3.22.1)

project(printerdriver_rn LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# This file lives at wrappers/react-native/android/. The package root is one directory up;
# the SDK sources are either staged inside it (npm tarball) or two directories above it
# (git checkout). Same rule as printerdriver-react-native.podspec.
get_filename_component(PD_PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${PD_PACKAGE_ROOT}/native/core/src")
  set(PD_SDK_ROOT "${PD_PACKAGE_ROOT}/native")
else()
  get_filename_component(PD_SDK_ROOT "${PD_PACKAGE_ROOT}/../.." ABSOLUTE)
endif()
message(STATUS "printerdriver: compiling the SDK from ${PD_SDK_ROOT}")

# Listed one by one rather than globbed, because two files in core/src must NOT be here:
#   - transport_win.cpp and platform_file_win.cpp are the Windows alternatives to
#     transport.cpp and platform_file_posix.cpp (the root CMakeLists.txt picks one pair or
#     the other, never both);
#   - transport_bluez.cpp is guarded by __linux__, WHICH ANDROID DEFINES, and it includes
#     <bluetooth/bluetooth.h> from BlueZ -- a header the NDK does not ship. Android
#     Bluetooth reaches the core through pd_add_printer_custom instead, which in this
#     package means a JavaScript transport (src/transports.ts).
set(PD_CORE_SOURCES
    ${PD_SDK_ROOT}/core/src/types.cpp
    ${PD_SDK_ROOT}/core/src/code_page.cpp
    ${PD_SDK_ROOT}/core/src/escpos_encoder.cpp
    ${PD_SDK_ROOT}/core/src/response_parser.cpp
    ${PD_SDK_ROOT}/core/src/capability_profile.cpp
    ${PD_SDK_ROOT}/core/src/job_store.cpp
    ${PD_SDK_ROOT}/core/src/transport.cpp
    ${PD_SDK_ROOT}/core/src/driver.cpp
    ${PD_SDK_ROOT}/core/src/star.cpp
    ${PD_SDK_ROOT}/core/src/epos.cpp
    ${PD_SDK_ROOT}/core/src/probe_path.cpp
    ${PD_SDK_ROOT}/core/src/transport_serial.cpp
    ${PD_SDK_ROOT}/core/src/device_profiles.cpp
    ${PD_SDK_ROOT}/core/src/identity.cpp
    ${PD_SDK_ROOT}/core/src/capability_probe.cpp
    ${PD_SDK_ROOT}/core/src/discovery.cpp
    ${PD_SDK_ROOT}/core/src/transport_custom.cpp
    ${PD_SDK_ROOT}/core/src/cash_drawer.cpp
    ${PD_SDK_ROOT}/core/src/auto_detect.cpp
    ${PD_SDK_ROOT}/core/src/registrations.cpp
    ${PD_SDK_ROOT}/core/src/platform_file_posix.cpp)

# pd_queue_* binds the print-queue addon (docs/sdk-spec.md §12).
set(PD_QUEUE_SOURCES ${PD_SDK_ROOT}/queue/src/print_queue.cpp)

# pd_self_test binds Printer::selfTest, which is defined in the receipt-DSL library because
# the diagnostic ticket is a DSL document (docs/api.md §15).
file(GLOB PD_DSL_SOURCES ${PD_SDK_ROOT}/dsl/src/*.cpp)

# The C ABI every wrapper binds. Its static_asserts are what keep this package's
# hand-mirrored TypeScript enums honest against the core.
set(PD_CAPI_SOURCES ${PD_SDK_ROOT}/capi/src/pd_capi.cpp)

set(PD_MODULE_SOURCES ${PD_PACKAGE_ROOT}/cpp/PrinterDriverModule.cpp)

add_library(printerdriver_rn SHARED
    ${PD_CORE_SOURCES}
    ${PD_QUEUE_SOURCES}
    ${PD_DSL_SOURCES}
    ${PD_CAPI_SOURCES}
    ${PD_MODULE_SOURCES})

target_include_directories(printerdriver_rn PUBLIC
    ${PD_PACKAGE_ROOT}/cpp
    ${PD_SDK_ROOT}/core/include
    ${PD_SDK_ROOT}/queue/include
    ${PD_SDK_ROOT}/dsl/include
    ${PD_SDK_ROOT}/capi/include
    # pd_internal.hpp, included by pd_capi.cpp only. cpp/PrinterDriverModule.cpp binds pd.h
    # and nothing else -- see its header comment.
    ${PD_SDK_ROOT}/capi/src)

# POSIX threads: the core's transport/worker-thread model (pd.h "Callback threads").
find_package(Threads REQUIRED)
target_link_libraries(printerdriver_rn PRIVATE Threads::Threads)

# React Native supplies jsi and the TurboModule runtime. When this file is included by the
# app's own CMake build (react-native.config.js "cxxModuleCMakeListsPath"), those targets
# already exist; when it is configured standalone they do not, and this is the point at
# which that shows up.
if(TARGET ReactAndroid::reactnative)
  target_link_libraries(printerdriver_rn PRIVATE ReactAndroid::reactnative ReactAndroid::jsi)
elseif(TARGET ReactAndroid::react_nativemodule_core)
  target_link_libraries(printerdriver_rn
      PRIVATE ReactAndroid::react_nativemodule_core ReactAndroid::jsi)
else()
  message(WARNING
    "printerdriver: no ReactAndroid target is defined. This CMakeLists.txt is written to be "
    "included by React Native's own Android build (see react-native.config.js); configured "
    "on its own it will not find <jsi/jsi.h> or <ReactCommon/TurboModule.h>.")
endif()

if(MSVC)
  target_compile_options(printerdriver_rn PRIVATE /W4)
else()
  target_compile_options(printerdriver_rn PRIVATE -Wall -Wextra)
endif()
