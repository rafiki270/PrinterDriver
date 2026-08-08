// swift-tools-version:5.9

import PackageDescription

// The manifest lives at the repository root so the SDK is consumable straight from its
// git URL. The C++ core is not vendored or duplicated for SwiftPM: these targets compile
// the same core/src and capi/src files CMakeLists.txt does, with the same C++17 standard,
// so the Swift wrapper can never drift onto a private copy of the engine.
let package = Package(
  name: "PrinterDriver",
  platforms: [
    .macOS(.v13),
    .iOS(.v16),
  ],
  products: [
    .library(name: "PrinterDriver", targets: ["PrinterDriver"])
  ],
  targets: [
    // The engine plus its C ABI, as one compilation unit set. capi/include is the public
    // headers directory and carries a hand-written module.modulemap that exposes pd.h
    // alone, so Swift sees a pure C module and no C++ ever crosses the language boundary.
    .target(
      name: "CPrinterDriver",
      path: ".",
      sources: [
        "core/src",
        "capi/src",
      ],
      publicHeadersPath: "capi/include",
      cxxSettings: [
        .headerSearchPath("core/include"),
        .headerSearchPath("capi/src"),
      ]
    ),

    // The wrapper: enum bridging and async adapters over the ABI above. No printing
    // logic — see wrappers/swift/README.md.
    .target(
      name: "PrinterDriver",
      dependencies: ["CPrinterDriver"],
      path: "wrappers/swift/Sources/PrinterDriver"
    ),

    // Test-only. Supplies the in-process scripted device (a C++ transport factory that
    // cannot be described in C) and the C++-side enum bridge. Depended on by the XCTest
    // bundle only, exactly as CMake compiles it into test_capi and nothing else.
    .target(
      name: "CPrinterDriverTestSupport",
      dependencies: ["CPrinterDriver"],
      path: ".",
      sources: [
        "capi/tests/pd_test_support.cpp"
      ],
      publicHeadersPath: "capi/tests",
      cxxSettings: [
        .headerSearchPath("core/include"),
        .headerSearchPath("core/tests"),
        .headerSearchPath("capi/src"),
      ]
    ),

    .testTarget(
      name: "PrinterDriverTests",
      dependencies: [
        "PrinterDriver",
        "CPrinterDriverTestSupport",
      ],
      path: "wrappers/swift/Tests/PrinterDriverTests"
    ),
  ],
  cxxLanguageStandard: .cxx17
)
