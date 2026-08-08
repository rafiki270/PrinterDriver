#include "pd_test_support.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "fake_printer.hpp"
#include "pd_internal.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/job_store.hpp"
#include "printerdriver/types.hpp"

// Test-only companions to pd.h; see pd_test_support.h for the contract. Compiled only
// into test_capi, never into printerdriver_capi.

namespace {

// Keyed on the pd_printer handle attachPrinter() mints, so pd_test_print_data_bytes and
// friends can reach the FakePrinter behind a printer that pd.h only ever exposes as an
// opaque pointer. Every test in this binary queries a handle before the driver that owns
// it is destroyed, so a later pd_destroy() never leaves a query reading through a stale
// entry, even though this table itself outlives any one driver.
std::mutex g_scripted_mutex;
std::unordered_map<pd_printer*, std::shared_ptr<pdfake::FakePrinter>> g_scripted;

std::shared_ptr<pdfake::FakePrinter> lookupDevice(pd_printer* printer) {
  std::lock_guard<std::mutex> lock(g_scripted_mutex);
  const auto it = g_scripted.find(printer);
  return it == g_scripted.end() ? nullptr : it->second;
}

}  // namespace

extern "C" pd_printer* pd_add_printer_scripted(pd_driver* driver, const char* printer_id,
                                               const char* script_id) {
  if (driver == nullptr || script_id == nullptr) {
    return nullptr;
  }
  const std::string id(script_id);

  pdfake::MockLink link;
  pd::CapabilityProfile profile;

  if (id == "ok") {
    // Healthy GS ( H printer, default (healthy) script: preflight passes, both the
    // print and cut markers are echoed, and the post-cut cutter read comes back clean.
    profile = pdfake::fastProfile(pd::CompletionMechanism::GsParenH);
  } else if (id == "gsr1") {
    // Healthy printer, but only the queued GS r 1 fence: a second GS r 1 after the cut
    // is an ordered fence, not a documented cutter guarantee, so this caps at
    // CutProcessed rather than CutFaultFree.
    profile = pdfake::fastProfile(pd::CompletionMechanism::GsR1);
  } else if (id == "silent") {
    // Preflight still passes; the payload still reaches the device; the process-ID
    // marker is simply never echoed back, so completion can never be confirmed.
    profile = pdfake::fastProfile(pd::CompletionMechanism::GsParenH);
    pdfake::Script script;
    script.answer_process_id = false;
    link.device->setScript(script);
  } else if (id == "paperout") {
    // DLE EOT 4 reports paper out, so strict preflight refuses the job before a single
    // payload byte is written.
    profile = pdfake::fastProfile(pd::CompletionMechanism::GsParenH);
    pdfake::Script script;
    script.paper_sensor = pdfake::paperOutByte();
    link.device->setScript(script);
  } else if (id == "refuse") {
    // The transport itself never connects.
    profile = pdfake::fastProfile(pd::CompletionMechanism::GsParenH);
    link.behaviour.connect_fails = true;
  } else {
    return nullptr;
  }

  pd::PrinterConfig config;
  if (printer_id != nullptr) {
    config.id = printer_id;
  }
  config.width_dots = pd::escpos::kWidth80mm;
  config.profile = profile;
  // Built last: MockLink::factory() snapshots `behaviour` by value, so it must run
  // after every script_id branch above has finished configuring the link.
  config.transport = link.factory();

  std::shared_ptr<pdfake::FakePrinter> device = link.device;
  pd_printer* printer = pd::capi::attachPrinter(driver, std::move(config));
  if (printer == nullptr) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(g_scripted_mutex);
  g_scripted[printer] = std::move(device);
  return printer;
}

extern "C" size_t pd_test_print_data_bytes(pd_printer* printer) {
  const std::shared_ptr<pdfake::FakePrinter> device = lookupDevice(printer);
  return device ? device->printDataBytes() : 0;
}

extern "C" size_t pd_test_cuts(pd_printer* printer) {
  const std::shared_ptr<pdfake::FakePrinter> device = lookupDevice(printer);
  return device ? device->cuts() : 0;
}

extern "C" int pd_test_received_contains(pd_printer* printer, const char* needle) {
  if (needle == nullptr) {
    return 0;
  }
  const std::shared_ptr<pdfake::FakePrinter> device = lookupDevice(printer);
  return device && device->receivedContains(needle) ? 1 : 0;
}

// --- Enum bridge ---------------------------------------------------------------------
//
// JobState, ConfidenceLevel, DeviceEvent, FailureReason and JobOutcome are counted from
// the core's own kAll* arrays (types.hpp): if a member is added there and pd.h is not
// updated to match, this count moves and pd.h's _COUNT does not. The other eight enums
// expose no kAll* array to this translation unit — CutSetting and PreflightMode have
// neither an array nor a to_string; PayloadKind, CompletionMechanism and CutVariant have
// a to_string but no public array; Alignment, CodePage and Binarization have neither —
// so their counts and value tables are hand-maintained here, independently of the
// static_asserts in pd_capi.cpp, which is the entire point of a second, human-written
// copy.

extern "C" int pd_test_cpp_enum_count(pd_test_enum which) {
  switch (which) {
    case PD_TEST_ENUM_JOB_STATE: return static_cast<int>(pd::kAllJobStates.size());
    case PD_TEST_ENUM_CONFIDENCE: return static_cast<int>(pd::kAllConfidenceLevels.size());
    case PD_TEST_ENUM_DEVICE_EVENT: return static_cast<int>(pd::kAllDeviceEvents.size());
    case PD_TEST_ENUM_FAILURE_REASON: return static_cast<int>(pd::kAllFailureReasons.size());
    case PD_TEST_ENUM_JOB_OUTCOME: return static_cast<int>(pd::kAllJobOutcomes.size());
    case PD_TEST_ENUM_CUT: return 4;             // CutSetting: Profile, Partial, Full, None
    case PD_TEST_ENUM_PREFLIGHT: return 2;        // PreflightMode: Strict, Skip
    case PD_TEST_ENUM_PAYLOAD_KIND: return 3;     // PayloadKind: Raster, Document, Raw
    case PD_TEST_ENUM_COMPLETION: return 3;       // CompletionMechanism: GsParenH, GsR1, None
    case PD_TEST_ENUM_CUT_VARIANT: return 3;      // CutVariant: Partial, Full, None
    case PD_TEST_ENUM_ALIGNMENT: return 3;        // escpos::Alignment: Left, Center, Right
    case PD_TEST_ENUM_CODE_PAGE: return 5;        // escpos::CodePage, non-contiguous values
    case PD_TEST_ENUM_BINARIZATION: return 2;     // escpos::Binarization
    case PD_TEST_ENUM_TOTAL: break;
  }
  return -1;
}

extern "C" int pd_test_cpp_enum_value(pd_test_enum which, int index) {
  const int count = pd_test_cpp_enum_count(which);
  if (index < 0 || count < 0 || index >= count) {
    return -1;
  }
  switch (which) {
    case PD_TEST_ENUM_JOB_STATE:
      return static_cast<int>(pd::kAllJobStates[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_CONFIDENCE:
      return static_cast<int>(pd::kAllConfidenceLevels[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_DEVICE_EVENT:
      return static_cast<int>(pd::kAllDeviceEvents[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_FAILURE_REASON:
      return static_cast<int>(pd::kAllFailureReasons[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_JOB_OUTCOME:
      return static_cast<int>(pd::kAllJobOutcomes[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_CUT: {
      static constexpr pd::CutSetting kValues[] = {pd::CutSetting::Profile,
                                                   pd::CutSetting::Partial,
                                                   pd::CutSetting::Full, pd::CutSetting::None};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_PREFLIGHT: {
      static constexpr pd::PreflightMode kValues[] = {pd::PreflightMode::Strict,
                                                       pd::PreflightMode::Skip};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_PAYLOAD_KIND: {
      static constexpr pd::PayloadKind kValues[] = {
          pd::PayloadKind::Raster, pd::PayloadKind::Document, pd::PayloadKind::Raw};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_COMPLETION: {
      static constexpr pd::CompletionMechanism kValues[] = {pd::CompletionMechanism::GsParenH,
                                                             pd::CompletionMechanism::GsR1,
                                                             pd::CompletionMechanism::None};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_CUT_VARIANT: {
      static constexpr pd::CutVariant kValues[] = {pd::CutVariant::Partial, pd::CutVariant::Full,
                                                    pd::CutVariant::None};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_ALIGNMENT: {
      static constexpr pd::escpos::Alignment kValues[] = {pd::escpos::Alignment::Left,
                                                           pd::escpos::Alignment::Center,
                                                           pd::escpos::Alignment::Right};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_CODE_PAGE: {
      static constexpr pd::escpos::CodePage kValues[] = {
          pd::escpos::CodePage::PC437, pd::escpos::CodePage::PC850,
          pd::escpos::CodePage::WPC1252, pd::escpos::CodePage::PC852,
          pd::escpos::CodePage::PC858};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_BINARIZATION: {
      static constexpr pd::escpos::Binarization kValues[] = {
          pd::escpos::Binarization::FixedThreshold, pd::escpos::Binarization::FloydSteinberg};
      return static_cast<int>(kValues[index]);
    }
    case PD_TEST_ENUM_TOTAL: break;
  }
  return -1;
}

extern "C" const char* pd_test_cpp_enum_name(pd_test_enum which, int index) {
  const int value = pd_test_cpp_enum_value(which, index);
  if (value < 0) {
    return nullptr;
  }
  switch (which) {
    case PD_TEST_ENUM_JOB_STATE:
      return pd::to_string(static_cast<pd::JobState>(value));
    case PD_TEST_ENUM_CONFIDENCE:
      return pd::to_string(static_cast<pd::ConfidenceLevel>(value));
    case PD_TEST_ENUM_DEVICE_EVENT:
      return pd::to_string(static_cast<pd::DeviceEvent>(value));
    case PD_TEST_ENUM_FAILURE_REASON:
      return pd::to_string(static_cast<pd::FailureReason>(value));
    case PD_TEST_ENUM_JOB_OUTCOME:
      return pd::to_string(static_cast<pd::JobOutcome>(value));
    case PD_TEST_ENUM_PAYLOAD_KIND:
      return pd::to_string(static_cast<pd::PayloadKind>(value));
    case PD_TEST_ENUM_COMPLETION:
      return pd::to_string(static_cast<pd::CompletionMechanism>(value));
    case PD_TEST_ENUM_CUT_VARIANT:
      return pd::to_string(static_cast<pd::CutVariant>(value));
    case PD_TEST_ENUM_CUT:
    case PD_TEST_ENUM_PREFLIGHT:
    case PD_TEST_ENUM_ALIGNMENT:
    case PD_TEST_ENUM_CODE_PAGE:
    case PD_TEST_ENUM_BINARIZATION:
    case PD_TEST_ENUM_TOTAL:
      return nullptr;
  }
  return nullptr;
}

extern "C" const char* pd_test_enum_label(pd_test_enum which) {
  switch (which) {
    case PD_TEST_ENUM_JOB_STATE: return "JobState";
    case PD_TEST_ENUM_CONFIDENCE: return "ConfidenceLevel";
    case PD_TEST_ENUM_DEVICE_EVENT: return "DeviceEvent";
    case PD_TEST_ENUM_FAILURE_REASON: return "FailureReason";
    case PD_TEST_ENUM_JOB_OUTCOME: return "JobOutcome";
    case PD_TEST_ENUM_CUT: return "CutSetting";
    case PD_TEST_ENUM_PREFLIGHT: return "PreflightMode";
    case PD_TEST_ENUM_PAYLOAD_KIND: return "PayloadKind";
    case PD_TEST_ENUM_COMPLETION: return "CompletionMechanism";
    case PD_TEST_ENUM_CUT_VARIANT: return "CutVariant";
    case PD_TEST_ENUM_ALIGNMENT: return "Alignment";
    case PD_TEST_ENUM_CODE_PAGE: return "CodePage";
    case PD_TEST_ENUM_BINARIZATION: return "Binarization";
    case PD_TEST_ENUM_TOTAL: break;
  }
  return "UnknownEnum";
}
