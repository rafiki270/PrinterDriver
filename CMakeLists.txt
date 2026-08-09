cmake_minimum_required(VERSION 3.16)

project(printerdriver
  VERSION 0.1.0
  DESCRIPTION "Cross-platform receipt printing SDK core"
  LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C11 is here for one target only: the ABI test, which must compile as plain C so that
# pd.h is proven to be a C header rather than C++ that happens to parse.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

function(printerdriver_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# The core has exactly two platform edges: sockets and durable file writes
# (docs/platforms.md, "Using the SDK from a Windows app"). Each is a pair of
# alternative translation units, one of which is compiled — never both — so the POSIX
# implementation is entirely unaffected by the existence of the Windows one.
#
# NOTE ON STATUS: the *_win.cpp files below have been syntax- and type-checked only
# (scripts/check_windows_syntax.sh). They have never been compiled by MSVC or clang-cl,
# never linked, never run. .github/workflows/windows.yml is the manual-dispatch job that
# will produce the first real evidence; it has not been run either.
if(WIN32)
  set(printerdriver_platform_sources
    core/src/transport_win.cpp
    core/src/platform_file_win.cpp)
else()
  set(printerdriver_platform_sources
    core/src/transport.cpp
    core/src/platform_file_posix.cpp)
endif()

# Standard library only. No FetchContent, no find_package, nothing downloaded:
# the core has to build inside iOS, Android, Flutter and Linux-agent toolchains
# without a package manager anywhere in the loop.
add_library(printerdriver_core STATIC
  core/src/types.cpp
  core/src/code_page.cpp
  core/src/escpos_encoder.cpp
  core/src/response_parser.cpp
  core/src/capability_profile.cpp
  # M14 — the cash-drawer peripheral facet, its state machine and the persisted
  # polarity calibration (docs/cash-drawer.md).
  core/src/cash_drawer.cpp
  core/src/device_profiles.cpp
  core/src/identity.cpp
  core/src/capability_probe.cpp
  core/src/job_store.cpp
  core/src/discovery.cpp
  core/src/driver.cpp
  # M16 — custom method registration (docs/api.md §16): the five per-driver registries
  # (completion, probe step, drawer kick, formatter, block handler) and their id/validation
  # rules. Consumed by the engine (driver.cpp), the probe (capability_probe.cpp) and the
  # DSL (format/render), so it sits in the core every layer already links.
  core/src/registrations.cpp
  # M15 — auto-detection (docs/api.md §15): discovery + identify + the printless probe
  # subset, composed, plus the report structure the self-test shares with it. Nothing
  # here prints and nothing here fires.
  core/src/auto_detect.cpp
  # Platform-independent, unlike the pair selected above: the embedder-owned transport
  # contains no socket call at all, because the socket belongs to the embedder
  # (docs/compatibility-brief.md §25). This is how Bluetooth reaches the core on Apple
  # and Android without either framework entering it.
  core/src/transport_custom.cpp
  # BlueZ RFCOMM. The whole file is inside `#ifdef __linux__`, so it is an empty
  # translation unit everywhere else and is listed unconditionally — which also keeps
  # SwiftPM, which compiles every file in core/src, honest. Syntax-checked only; see
  # scripts/check_linux_bluetooth_syntax.sh and the note at the top of the file.
  core/src/transport_bluez.cpp
  # --- M13b (docs/wire-protocols.md) -------------------------------------------------
  # Star Line Mode: the two documented raw fences, the line-mode encoder and the
  # ESC/POS -> Star transcoder (§2).
  core/src/star.cpp
  # The Epson ePOS-Print service: SOAP envelopes, namespace-aware parsing, the spooler's
  # accepted-not-printed discipline and JobID retrieval (§1). Its HTTP client is plain
  # sockets through net_platform.hpp — no dependency enters the core for it.
  core/src/epos.cpp
  # Does a print server in the path pass the printer's answers back? (§19-§23 of
  # docs/compatibility-brief.md.) Behind `pdctl probe-path`.
  core/src/probe_path.cpp
  # RS-232 over POSIX termios, with its own Windows branch inside the file for the same
  # reason transport_bluez.cpp has one.
  core/src/transport_serial.cpp
  ${printerdriver_platform_sources})

target_include_directories(printerdriver_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>)
# platform_file.hpp is an internal header: core/src is on the include path for anything
# that compiles the core's own sources, and is not exported to consumers.
target_include_directories(printerdriver_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/src)

# Sockets and one thread per printer plus one per connection: the only OS services the
# core uses. On Windows the socket library is not in the default link set.
find_package(Threads REQUIRED)
target_link_libraries(printerdriver_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(printerdriver_core PUBLIC ws2_32)
endif()

printerdriver_warnings(printerdriver_core)

# Print-queue addon (docs/sdk-spec.md §12). Its own library, because holding, expiry
# and priority are policy: an app that does not want them must not link them.
add_library(printerdriver_queue STATIC
  queue/src/print_queue.cpp)

target_include_directories(printerdriver_queue PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/queue/include>)
target_link_libraries(printerdriver_queue PUBLIC printerdriver_core)

printerdriver_warnings(printerdriver_queue)

# Receipt DSL (docs/receipt-dsl.md) — the document tier. Its own library for the same
# reason the queue is: an app that renders its own receipts must not link a template
# engine it never calls. Depends on the core for the encoder and the media profile, and
# on nothing else — the strict JSON subset it needs is written by hand in dsl/src.
add_library(printerdriver_dsl STATIC
  dsl/src/text.cpp
  dsl/src/json.cpp
  dsl/src/report.cpp
  dsl/src/document.cpp
  dsl/src/format.cpp
  dsl/src/bind.cpp
  dsl/src/barcode.cpp
  dsl/src/render.cpp
  # M19 — parse → bind → render as one call (docs/api.md §3, §17.1). pdctl's template
  # path and the ABI's pd_render_document / pd_print_document_json are the same three
  # steps in the same order; sharing them here is what keeps the CLI and every wrapper
  # from disagreeing about what a document means.
  dsl/src/pipeline.cpp
  # M15 — Printer::selfTest (docs/api.md §15). Declared in core/driver.hpp, defined here
  # because the diagnostic ticket is a DSL document and the DSL depends on the core, not
  # the other way round. See the linkage note at the top of printerdriver/self_test.hpp.
  dsl/src/self_test.cpp)

target_include_directories(printerdriver_dsl PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dsl/include>)
target_link_libraries(printerdriver_dsl PUBLIC printerdriver_core)

printerdriver_warnings(printerdriver_dsl)

# C ABI (docs/api.md §9) — what every other wrapper binds.
add_library(printerdriver_capi STATIC
  capi/src/pd_capi.cpp)

target_include_directories(printerdriver_capi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi/include>)
target_include_directories(printerdriver_capi PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/capi/src)
# M13b: pd_queue_* binds the print-queue addon (docs/sdk-spec.md §12). The addon stays a
# separate library — an app that does not want holding, expiry and priority still must not
# have to link a policy engine — but the ABI carries it, because a wrapper cannot
# reimplement the three §12 rules without eventually getting one of them wrong.
# printerdriver_queue PUBLIC-links the core, so naming both here would put the same
# archive on the link line twice.
#
# M15: the DSL joins it, because pd_self_test binds Printer::selfTest and that member is
# defined in the DSL library (docs/api.md §15 — the ticket is a document). The ABI is
# what every wrapper binds, so the self-test has to be reachable through it.
target_link_libraries(printerdriver_capi PUBLIC printerdriver_queue printerdriver_dsl)

printerdriver_warnings(printerdriver_capi)

# The same ABI as a shared library, for wrappers that dlopen it instead of linking it
# (wrappers/dart binds this one through dart:ffi). Off by default: the artifact this
# project ships is the static library above, and an FFI consumer opts in explicitly.
option(PD_BUILD_SHARED_CAPI
  "Also build the C ABI as a shared library for FFI wrappers (Dart, Python, ...)" OFF)

if(PD_BUILD_SHARED_CAPI)
  # The core is static and gets linked into a shared object here, so it has to be
  # position-independent. Scoped to this option so the default build is untouched.
  set_property(TARGET printerdriver_core PROPERTY POSITION_INDEPENDENT_CODE ON)
  # M13b: the queue addon is linked into the same shared object now that pd_queue_*
  # exists, so it needs the same treatment.
  set_property(TARGET printerdriver_queue PROPERTY POSITION_INDEPENDENT_CODE ON)
  # M15: and so does the DSL, now that pd_self_test binds a member defined there.
  set_property(TARGET printerdriver_dsl PROPERTY POSITION_INDEPENDENT_CODE ON)

  add_library(printerdriver_capi_shared SHARED capi/src/pd_capi.cpp)
  set_target_properties(printerdriver_capi_shared PROPERTIES
    OUTPUT_NAME printerdriver_capi)
  target_include_directories(printerdriver_capi_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi/include>)
  target_include_directories(printerdriver_capi_shared PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/capi/src)
  target_link_libraries(printerdriver_capi_shared
    PRIVATE printerdriver_queue printerdriver_dsl)  # M13b, M15
  printerdriver_warnings(printerdriver_capi_shared)

  # Same ABI plus the scripted device and the enum bridge, so a wrapper's test suite can
  # drive a printer that does not exist. A second target rather than a build flag on the
  # first one: the library that ships must not contain a test double.
  add_library(printerdriver_capi_testing SHARED
    capi/src/pd_capi.cpp
    capi/tests/pd_test_support.cpp)
  target_include_directories(printerdriver_capi_testing PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi/include>)
  target_include_directories(printerdriver_capi_testing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/capi/src
    ${CMAKE_CURRENT_SOURCE_DIR}/capi/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
  target_link_libraries(printerdriver_capi_testing
    PRIVATE printerdriver_queue printerdriver_dsl)  # M13b, M15
  printerdriver_warnings(printerdriver_capi_testing)
endif()

# The local printer agent (docs/techspec.md §5, docs/sdk-spec.md §14) — the same core
# compiled as a daemon, with a hand-rolled HTTP/1.1 front end. A library plus a thin
# main, so the test suite can spin the real server on a loopback port in-process.
add_library(printerdriver_agent STATIC
  agent/src/http.cpp
  agent/src/agent.cpp
  # M13b: the Star CloudPRNT server (docs/wire-protocols.md §2). The printer polls us, so
  # it cannot go through the byte-transport engine at all — a pulled job has no socket to
  # fence — and it keeps its own job repository with its own retain-until-confirmed rules.
  agent/src/cloudprnt.cpp)

target_include_directories(printerdriver_agent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/agent/include>)
target_link_libraries(printerdriver_agent PUBLIC printerdriver_dsl)

printerdriver_warnings(printerdriver_agent)

add_executable(pd-agent agent/src/main.cpp)
target_link_libraries(pd-agent PRIVATE printerdriver_agent)
printerdriver_warnings(pd-agent)

add_executable(pdctl tools/pdctl.cpp)
target_link_libraries(pdctl PRIVATE printerdriver_dsl)
printerdriver_warnings(pdctl)

enable_testing()

set(printerdriver_tests
  test_types
  test_encoder
  test_codepage
  test_raster
  test_qr
  test_parser
  test_store
  test_identity
  test_profiles
  test_discovery
  test_engine
  # --- M13b ---------------------------------------------------------------------------
  test_star        # docs/wire-protocols.md §2: fences, counter packing, transcoding
  test_epos        # docs/wire-protocols.md §1: envelopes, parsing, spooler discipline
  test_serial      # RS-232 over a pty pair, driving the same scripted device
  test_probe_path  # docs/compatibility-brief.md §19-§23: PRINT_SERVER_ONLY
  # M14 — docs/cash-drawer.md: the fleet's drawer facets, the state machine and the
  # verified opening sequence against the scripted device.
  test_drawer
  # M15 — docs/api.md §15. Auto-detection against three real loopback listeners: one
  # ESC/POS-answering, one silent, one refusing. Core-only on purpose — autoDetect
  # composes discovery, identify and the printless probe and needs no renderer.
  test_autodetect)

foreach(test_name IN LISTS printerdriver_tests)
  add_executable(${test_name}
    core/tests/${test_name}.cpp
    core/tests/test_harness.cpp)
  target_link_libraries(${test_name} PRIVATE printerdriver_core)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
  printerdriver_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The DSL suites reuse the core's assert harness; they need no printer at all, because
# the whole layer is a pure function of (document, model, profile).
set(printerdriver_dsl_tests
  test_dsl_json
  test_dsl_document
  test_dsl_format
  test_dsl_bind
  test_dsl_barcode
  test_dsl_render
  # M15 — docs/api.md §15. Printer::selfTest is defined in this library, so its suite
  # lives here; it drives the core's scripted device, hence the core/tests include path
  # the loop below already adds.
  test_selftest)

foreach(test_name IN LISTS printerdriver_dsl_tests)
  add_executable(${test_name}
    dsl/tests/${test_name}.cpp
    core/tests/test_harness.cpp)
  target_link_libraries(${test_name} PRIVATE printerdriver_dsl)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
  printerdriver_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The queue addon reuses the core's scripted device and mock transport rather than
# growing a second fake printer.
add_executable(test_queue
  queue/tests/test_queue.cpp
  core/tests/test_harness.cpp)
target_link_libraries(test_queue PRIVATE printerdriver_queue)
target_include_directories(test_queue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
printerdriver_warnings(test_queue)
add_test(NAME test_queue COMMAND test_queue)

# The agent suite runs the real HTTP server on a loopback port against the same scripted
# device the engine tests use: an evidence document that never crossed a socket would
# prove nothing about the socket.
add_executable(test_agent
  agent/tests/test_agent.cpp
  core/tests/test_harness.cpp)
target_link_libraries(test_agent PRIVATE printerdriver_agent)
target_include_directories(test_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
printerdriver_warnings(test_agent)
add_test(NAME test_agent COMMAND test_agent)

# M13b. The CloudPRNT contract, walked by a scripted HTTP printer client over real
# loopback sockets: poll -> get -> delete, plus the re-download and never-consume-on-GET
# rules that a server which only ever handled the happy path would silently break.
add_executable(test_cloudprnt
  agent/tests/test_cloudprnt.cpp
  core/tests/test_harness.cpp)
target_link_libraries(test_cloudprnt PRIVATE printerdriver_agent)
target_include_directories(test_cloudprnt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
printerdriver_warnings(test_cloudprnt)
add_test(NAME test_cloudprnt COMMAND test_cloudprnt)

# Compiled as C11 on purpose. pd_test_support.cpp is the only C++ in this target: it
# holds the scripted transport and the enum bridge, and it is deliberately not part of
# printerdriver_capi, so nothing shipped carries a test double.
add_executable(test_capi
  capi/tests/test_capi.c
  capi/tests/pd_test_support.cpp)
target_link_libraries(test_capi PRIVATE printerdriver_capi)
target_include_directories(test_capi PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/capi/src
  ${CMAKE_CURRENT_SOURCE_DIR}/capi/tests
  ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
printerdriver_warnings(test_capi)
add_test(NAME test_capi COMMAND test_capi)
