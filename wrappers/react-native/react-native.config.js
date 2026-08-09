/**
 * React Native CLI autolinking.
 *
 * The module is a pure C++ TurboModule with no Java, Kotlin or Objective-C half, so the
 * Android side needs the `cxxModule*` keys: they tell React Native's own CMake build to
 * include android/CMakeLists.txt, which library target it produces, and which header
 * declares the provider function it should call. iOS needs nothing beyond the podspec.
 *
 * UNVERIFIED: no React Native application exists on the machine this was written on, so
 * autolinking has never actually run against this file. README.md "Verification status"
 * lists it explicitly among the things a first real app build has to confirm, and
 * ios/PrinterDriverPackage.h documents the manual-registration escape hatch for the case
 * where it does not work first time.
 */

module.exports = {
  dependency: {
    platforms: {
      ios: {},
      android: {
        cmakeListsPath: 'CMakeLists.txt',
        cxxModuleCMakeListsModuleName: 'printerdriver_rn',
        cxxModuleCMakeListsPath: 'CMakeLists.txt',
        // The header that declares pdrn::PrinterDriverModuleProvider.
        cxxModuleHeaderName: 'PrinterDriverModule',
      },
    },
  },
};
