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

# Standard library only. No FetchContent, no find_package, nothing downloaded:
# the core has to build inside iOS, Android, Flutter and Linux-agent toolchains
# without a package manager anywhere in the loop.
add_library(printerdriver_core STATIC
  core/src/types.cpp
  core/src/code_page.cpp
  core/src/escpos_encoder.cpp
  core/src/response_parser.cpp
  core/src/capability_profile.cpp
  core/src/job_store.cpp
  core/src/transport.cpp
  core/src/driver.cpp)

target_include_directories(printerdriver_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>)

# POSIX sockets and one thread per printer plus one per connection: the only OS
# services the core uses.
find_package(Threads REQUIRED)
target_link_libraries(printerdriver_core PUBLIC Threads::Threads)

printerdriver_warnings(printerdriver_core)

# Print-queue addon (docs/sdk-spec.md §12). Its own library, because holding, expiry
# and priority are policy: an app that does not want them must not link them.
add_library(printerdriver_queue STATIC
  queue/src/print_queue.cpp)

target_include_directories(printerdriver_queue PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/queue/include>)
target_link_libraries(printerdriver_queue PUBLIC printerdriver_core)

printerdriver_warnings(printerdriver_queue)

# C ABI (docs/api.md §9) — what every other wrapper binds.
add_library(printerdriver_capi STATIC
  capi/src/pd_capi.cpp)

target_include_directories(printerdriver_capi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi/include>)
target_include_directories(printerdriver_capi PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/capi/src)
target_link_libraries(printerdriver_capi PUBLIC printerdriver_core)

printerdriver_warnings(printerdriver_capi)

add_executable(pdctl tools/pdctl.cpp)
target_link_libraries(pdctl PRIVATE printerdriver_core)
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
  test_engine)

foreach(test_name IN LISTS printerdriver_tests)
  add_executable(${test_name}
    core/tests/${test_name}.cpp
    core/tests/test_harness.cpp)
  target_link_libraries(${test_name} PRIVATE printerdriver_core)
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
