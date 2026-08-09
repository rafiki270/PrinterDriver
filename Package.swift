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
      // `path: "."` makes the whole repository this target's directory, so SwiftPM scans all
      // of it for RESOURCES even though `sources` is explicit. wrappers/ holds no C++ this
      // target compiles, and it does hold things SwiftPM would rather have an opinion about
      // -- notably the React Native package's node_modules, whose React Native copy carries
      // .lproj directories that SwiftPM reads as localized resources and then refuses to
      // build without a defaultLocalization. Excluding the directory keeps `swift test`
      // working in a checkout where someone has run `npm install`, which is every checkout
      // that touches the React Native wrapper.
      exclude: ["wrappers"],
      sources: [
        "core/src",
        "capi/src",
        // M13b: pd_queue_* binds the print-queue addon (docs/sdk-spec.md §12). CMake keeps
        // it a separate library so an app that does not want holding, expiry and priority
        // need not link a policy engine; SwiftPM has one target for the whole ABI, so the
        // addon's single translation unit joins it here.
        "queue/src",
        // M15: pd_self_test binds Printer::selfTest, and that member is defined in the
        // receipt-DSL library because the diagnostic ticket is a DSL document
        // (docs/api.md §15). CMake keeps the DSL a separate library and links it into the
        // ABI; SwiftPM has one target for the whole ABI, so its sources join it here.
        "dsl/src",
      ],
      publicHeadersPath: "capi/include",
      cxxSettings: [
        .headerSearchPath("core/include"),
        .headerSearchPath("capi/src"),
        .headerSearchPath("queue/include"),
        .headerSearchPath("dsl/include"),
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
      exclude: ["wrappers"],  // Same reason as above.
      sources: [
        "capi/tests/pd_test_support.cpp"
      ],
      publicHeadersPath: "capi/tests",
      cxxSettings: [
        .headerSearchPath("core/include"),
        .headerSearchPath("core/tests"),
        .headerSearchPath("capi/src"),
        .headerSearchPath("queue/include"),
        .headerSearchPath("dsl/include"),  // M15
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
