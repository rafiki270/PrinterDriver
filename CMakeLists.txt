cmake_minimum_required(VERSION 3.16)

project(printerdriver
  VERSION 0.1.0
  DESCRIPTION "Cross-platform receipt printing SDK core"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
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
  core/src/device_profiles.cpp
  core/src/identity.cpp
  core/src/capability_probe.cpp
  core/src/job_store.cpp
  core/src/transport.cpp
  core/src/driver.cpp)

target_include_directories(printerdriver_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>)

# POSIX sockets and one thread per printer plus one per connection: the only OS
# services the core uses.
find_package(Threads REQUIRED)
target_link_libraries(printerdriver_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(printerdriver_core PRIVATE /W4)
else()
  target_compile_options(printerdriver_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(pdctl tools/pdctl.cpp)
target_link_libraries(pdctl PRIVATE printerdriver_core)
if(MSVC)
  target_compile_options(pdctl PRIVATE /W4)
else()
  target_compile_options(pdctl PRIVATE -Wall -Wextra -Wpedantic)
endif()

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
  test_engine)

foreach(test_name IN LISTS printerdriver_tests)
  add_executable(${test_name}
    core/tests/${test_name}.cpp
    core/tests/test_harness.cpp)
  target_link_libraries(${test_name} PRIVATE printerdriver_core)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/tests)
  if(MSVC)
    target_compile_options(${test_name} PRIVATE /W4)
  else()
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
