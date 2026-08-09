// The iOS registration hook for the C++ TurboModule.
//
// Autolinking is expected to find pdrn::PrinterDriverModuleProvider on its own, through the
// codegen spec in src/NativePrinterDriver.ts and the podspec next to this file. This header
// exists for the case where it does not, and for apps that register their C++ TurboModules
// explicitly -- which is a supported and quite common arrangement:
//
//   #import <printerdriver-react-native/PrinterDriverPackage.h>
//
//   // In your RCTTurboModuleManagerDelegate:
//   - (std::shared_ptr<facebook::react::TurboModule>)
//         getTurboModule:(const std::string &)name
//              jsInvoker:(std::shared_ptr<facebook::react::CallInvoker>)jsInvoker {
//     if (auto module = PrinterDriverTurboModule(name, jsInvoker)) return module;
//     return nullptr;
//   }
//
// There is deliberately no Objective-C implementation of the module itself. The whole
// module is portable C++ and is the same translation unit on both platforms; an
// Objective-C++ half would be a second place for the two platforms to diverge.

#pragma once

#ifdef __cplusplus

#include <memory>
#include <string>

#ifndef PD_RN_TEST_STUB
#include <ReactCommon/CallInvoker.h>
#include <ReactCommon/TurboModule.h>
#endif

#include "PrinterDriverModule.h"

/// Returns the module when `name` is "PrinterDriver", and nullptr otherwise, so a delegate
/// can chain several providers.
inline std::shared_ptr<facebook::react::TurboModule> PrinterDriverTurboModule(
    const std::string& name,
    const std::shared_ptr<facebook::react::CallInvoker>& jsInvoker) {
  return pdrn::PrinterDriverModuleProvider(name, jsInvoker);
}

#endif  // __cplusplus
