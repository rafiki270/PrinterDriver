# The iOS half of the packaging. It compiles the portable C++ core, its C ABI, the queue
# addon and the receipt DSL together with cpp/PrinterDriverModule.cpp -- the same sources
# Package.swift and wrappers/android/CMakeLists.txt compile, never a private copy of the
# engine (docs/platforms.md, "React Native / Expo wrapper").
#
# SOURCE ROOT
#   Inside the git checkout the sources live two directories up. In a published npm tarball
#   they cannot: an npm package may not reference files above its own directory, so
#   scripts/stage-native-sources.mjs copies them into native/ during `npm pack`. Prefer the
#   staged copy when it is there, and the repository when it is not.
#
# UNVERIFIED: this podspec has never been evaluated by CocoaPods and never built by Xcode.
# There is no React Native application on the machine this package was written on. See
# README.md "Verification status" for the ordered list of what a first real `pod install`
# and build have to confirm.

require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))
staged = File.directory?(File.join(__dir__, "native", "core", "src"))
sdk = staged ? "native" : "../.."

Pod::Spec.new do |s|
  s.name         = "printerdriver-react-native"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = { :type => "MIT", :file => "LICENSE" }
  s.authors      = package["author"]

  s.platforms    = { :ios => "16.0" }
  s.source       = { :git => "https://github.com/rafiki270/PrinterDriver.git", :tag => "#{s.version}" }

  # The module plus the engine. core/src carries platform-alternative translation units
  # (transport_win.cpp, transport_bluez.cpp) that are wrapped in their own platform guards
  # and preprocess to nothing here, exactly as they do for Package.swift.
  s.source_files = [
    "cpp/**/*.{h,cpp}",
    "ios/**/*.{h,m,mm}",
    "#{sdk}/core/src/**/*.{cpp,hpp}",
    "#{sdk}/capi/src/**/*.{cpp,hpp}",
    "#{sdk}/queue/src/**/*.cpp",
    "#{sdk}/dsl/src/**/*.cpp",
  ]
  # cpp/__tests__/rn_stub.h is a host-side syntax-check fixture and must never reach a
  # device build; see its own header comment.
  s.exclude_files = "cpp/__tests__/**/*"

  s.public_header_files = "cpp/PrinterDriverModule.h", "ios/**/*.h"
  s.header_mappings_dir = "."

  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++17",
    "HEADER_SEARCH_PATHS" => [
      "\"$(PODS_TARGET_SRCROOT)/#{sdk}/core/include\"",
      "\"$(PODS_TARGET_SRCROOT)/#{sdk}/capi/include\"",
      # pd_internal.hpp is included by pd_capi.cpp only; cpp/PrinterDriverModule.cpp binds
      # pd.h and nothing else (see its header comment).
      "\"$(PODS_TARGET_SRCROOT)/#{sdk}/capi/src\"",
      "\"$(PODS_TARGET_SRCROOT)/#{sdk}/queue/include\"",
      "\"$(PODS_TARGET_SRCROOT)/#{sdk}/dsl/include\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp\"",
    ].join(" "),
    "DEFINES_MODULE" => "YES",
  }

  # The C++ TurboModule path needs the New Architecture. install_modules_dependencies wires
  # React-Core, ReactCommon/turbomodule/core and hermes/JSI for whatever React Native
  # version the app resolved, which is why no version is pinned here.
  if respond_to?(:install_modules_dependencies, true)
    install_modules_dependencies(s)
  else
    s.dependency "React-Core"
  end
end
