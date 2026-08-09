// printerdriver-react-native -- the C++ TurboModule.
//
// ONE implementation for both platforms. React Native's New Architecture supports C++
// TurboModules, which bind a C ABI directly and compile once for iOS and Android;
// capi/include/printerdriver/pd.h is already that ABI, so this file is a JSI translation
// layer and nothing else. There is no Objective-C half and no Kotlin half, and no printing
// logic lives here: everything that needs a decision -- fencing, dedupe, preflight,
// confidence grading, the refusal to overclaim -- is behind pd.h in the portable core that
// SwiftPM, the Android AAR and this package all compile from the same sources
// (docs/platforms.md, "React Native / Expo wrapper").
//
// ---------------------------------------------------------------------------------------
// THREADING -- the whole contract, because it is the part that can deadlock
// ---------------------------------------------------------------------------------------
//
// pd.h is explicit that core callbacks arrive on the core's own threads: job events on the
// owning printer's worker thread, device events on whichever thread decoded the status,
// custom-transport and custom-registration callbacks on the worker thread driving them.
// JavaScript is single-threaded and may only be touched on the JS thread. Two directions,
// two rules:
//
//   NATIVE -> JS (notifications: job events, device events, discovery candidates, logs)
//     Marshalled into plain C++ data on the core thread, then posted to the JS thread with
//     the TurboModule's CallInvoker (`invokeAsync`). Nothing blocks. By the time the JS
//     sink runs, the core thread is long gone -- which is exactly why pd_job_event arrives
//     BY VALUE in the ABI, and why this module copies every string out of a callback
//     struct before returning from it.
//
//   NATIVE -> JS -> NATIVE (questions: a JS transport's connect/write/close, a registered
//   completion method's fence and matcher, a probe step's classify, a block handler, a
//   formatter, a vendor drawer kick)
//     These have RETURN VALUES the core is waiting for. The core thread registers a pending
//     request, posts the question through the CallInvoker, and BLOCKS on a condition
//     variable until JavaScript calls `respond(requestId, value)` or a deadline passes.
//
// That second rule is only safe because of a third one:
//
//   NO BLOCKING pd_* CALL EVER RUNS ON THE JS THREAD. Every ABI function that blocks --
//   pd_destroy, pd_printer_drain, pd_printer_refresh_status, pd_job_await, pd_drawer_open,
//   pd_drawer_read_sensor, pd_self_test, pd_auto_detect, pd_discover, pd_queue_destroy --
//   is run on a worker thread here and surfaces to JavaScript as a Promise. If any of them
//   ran inline, a JS thread parked inside the ABI could be exactly the thread a core worker
//   is waiting on for a transport write, and the two would deadlock. The Promise-returning
//   surface in src/NativePrinterDriver.ts is not an ergonomic choice; it is this invariant.
//
// A missed deadline is answered with the REGISTRATION'S OWN documented failure -- a short
// write, a `notMine` verdict, a declined formatter, a degraded block -- never with an
// invented success, and never by leaving a printer hung on a buggy JS callback.
//
// ---------------------------------------------------------------------------------------
// HANDLES
// ---------------------------------------------------------------------------------------
// pd_driver*, pd_printer*, pd_job* and pd_queue* never cross into JavaScript as pointers.
// Each is registered in a table here and referred to by an int32 token; 0 is the null
// handle, which is how pd_find_job reports "no such key" without an exception. The same
// pd_job* always maps to the same token, so pd.h's idempotency-key dedupe stays visible as
// handle equality on the JavaScript side too.

#pragma once

#ifndef PD_RN_TEST_STUB
#include <ReactCommon/CallInvoker.h>
#include <ReactCommon/TurboModule.h>
#include <jsi/jsi.h>
#else
// See cpp/__tests__/rn_stub.h: host-side syntax checking only, force-included in place of
// the three headers above via `-DPD_RN_TEST_STUB -include`. The real iOS and Android
// builds never define PD_RN_TEST_STUB, so they always take the #ifndef branch; this #else
// changes nothing about the shipped module.
#endif

#include <memory>
#include <string>
#include <vector>

namespace pdrn {

/// Defined in PrinterDriverModule.cpp: the handle tables, the sink, the pending-request
/// map and everything the C callbacks need. Opaque here so this header stays readable.
struct ModuleState;

class PrinterDriverModule : public facebook::react::TurboModule {
 public:
  explicit PrinterDriverModule(std::shared_ptr<facebook::react::CallInvoker> jsInvoker);
  ~PrinterDriverModule() override;

  PrinterDriverModule(const PrinterDriverModule&) = delete;
  PrinterDriverModule& operator=(const PrinterDriverModule&) = delete;

  facebook::jsi::Value get(facebook::jsi::Runtime& runtime,
                           const facebook::jsi::PropNameID& propName) override;
  std::vector<facebook::jsi::PropNameID> getPropertyNames(
      facebook::jsi::Runtime& runtime) override;

 private:
  std::shared_ptr<ModuleState> state_;
};

/// The autolinking entry point. iOS registers it from
/// RCTAppSetupDefaultModuleFromClass/`RCTTurboModuleManagerDelegate`; Android from the
/// `cxxModuleCMakeListsPath` hook in react-native.config.js. Both hand over the same name
/// ("PrinterDriver") and the same CallInvoker.
std::shared_ptr<facebook::react::TurboModule> PrinterDriverModuleProvider(
    const std::string& name,
    const std::shared_ptr<facebook::react::CallInvoker>& jsInvoker);

}  // namespace pdrn
