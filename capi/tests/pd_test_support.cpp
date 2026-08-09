#include "pd_test_support.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fake_printer.hpp"
#include "pd_internal.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/job_store.hpp"
#include "printerdriver/self_test.hpp"
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
  } else if (id == "drawer") {
    // M14. A documented Epson-style 24 V port with a working switch, so the wrappers
    // can drive the whole opening sequence and see OPEN_VERIFIED come back.
    profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
    profile.drawer.status.polarity.calibrated = true;
    profile.drawer.status.polarity.high_means_open = true;
  } else if (id == "drawer-uncalibrated") {
    // The same hardware before anybody has measured which level means open.
    profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
  } else if (id == "drawer-locked") {
    // The pulse goes out and the switch never moves: a locked drawer, a jam, a wrong
    // channel or a cable that fits and is not wired for this printer.
    profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
    profile.drawer.status.polarity.calibrated = true;
    profile.drawer.status.polarity.high_means_open = true;
    pdfake::Script script;
    script.drawer_opens_on_kick = false;
    link.device->setScript(script);
  } else if (id == "drawer-unknown-port") {
    // An unclassified 6P6C socket. Nothing here is fired, however healthy the rest of
    // the printer looks — docs/cash-drawer.md's giant-letters rule.
    profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
    profile.drawer.electrical.standard = pd::DrawerPortStandard::Unknown;
  } else if (id == "vendor-idle") {
    // M16 (docs/api.md §16). A CompletionMechanism::VendorIdle profile bound to the
    // registered id "acme.x-idle", behind a device that echoes the ESC x fence. The whole
    // point of the milestone: a made-up vendor ack scheme drives a graded completion path
    // with no core release. The caller must have registered "acme.x-idle" first.
    profile = pdfake::vendorIdleProfile("acme.x-idle");
  } else if (id == "vendor-idle-busy") {
    // The same printer that never idles: the fence is sent, nothing is echoed, and the
    // job ends Unknown — the honest answer for a receipt whose fate cannot be established.
    profile = pdfake::vendorIdleProfile("acme.x-idle");
    pdfake::Script script;
    script.answer_vendor_idle = false;
    link.device->setScript(script);
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

// M14. How many ESC p pulses actually reached the device, which is the number a
// refusal test has to see stay at zero.
extern "C" size_t pd_test_drawer_kicks(pd_printer* printer) {
  const std::shared_ptr<pdfake::FakePrinter> device = lookupDevice(printer);
  return device ? device->drawerKicks() : 0;
}

extern "C" int pd_test_drawer_is_open(pd_printer* printer) {
  const std::shared_ptr<pdfake::FakePrinter> device = lookupDevice(printer);
  return device && device->drawerOpen() ? 1 : 0;
}

// Moves the physical drawer without going through the printer — an operator's hand,
// which is what the non-destructive polarity calibration asks for.
extern "C" void pd_test_set_drawer_open(pd_printer* printer, int open) {
  const std::shared_ptr<pdfake::FakePrinter> device = lookupDevice(printer);
  if (device) {
    device->setDrawerOpen(open != 0);
  }
}

// --- Scripted link behind a caller-supplied transport vtable -------------------------

struct pd_test_link {
  std::shared_ptr<pdfake::FakePrinter> device = std::make_shared<pdfake::FakePrinter>();
  pd_printer* printer = nullptr;

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::vector<uint8_t>> pending;
  bool stop = false;
  std::thread reader;

  std::atomic<bool> connect_succeeds{true};
  std::atomic<size_t> connects{0};
  std::atomic<size_t> closes{0};
  std::atomic<size_t> bytes_written{0};

  pd_test_link() {
    reader = std::thread([this] { readerLoop(); });
  }

  ~pd_test_link() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_one();
    reader.join();
  }

  void readerLoop() {
    for (;;) {
      std::vector<uint8_t> chunk;
      pd_printer* target = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stop || !pending.empty(); });
        if (stop && pending.empty()) {
          return;
        }
        chunk = std::move(pending.front());
        pending.erase(pending.begin());
        target = printer;
      }
      // Outside the lock, on this thread and never on the core's worker: exactly the
      // contract pd.h states for pd_transport_feed_bytes.
      if (target != nullptr) {
        pd_transport_feed_bytes(target, chunk.data(), chunk.size());
      }
    }
  }
};

extern "C" pd_test_link* pd_test_link_create(const char* script_id) {
  const std::string id(script_id != nullptr ? script_id : "");
  if (id != "ok" && id != "gsr1") {
    return nullptr;
  }
  auto* link = new pd_test_link();
  if (id == "gsr1") {
    pdfake::Script script;
    script.answer_process_id = false;  // queued fence only
    link->device->setScript(script);
  }
  return link;
}

extern "C" void pd_test_link_destroy(pd_test_link* link) { delete link; }

extern "C" void pd_test_link_bind(pd_test_link* link, pd_printer* printer) {
  if (link == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(link->mutex);
  link->printer = printer;
}

extern "C" void pd_test_link_refuse_connections(pd_test_link* link) {
  if (link != nullptr) {
    link->connect_succeeds.store(false);
  }
}

extern "C" int32_t pd_test_link_connect(pd_test_link* link) {
  if (link == nullptr) {
    return 0;
  }
  ++link->connects;
  return link->connect_succeeds.load() ? 1 : 0;
}

extern "C" int64_t pd_test_link_write(pd_test_link* link, const uint8_t* data,
                                      size_t size) {
  if (link == nullptr || data == nullptr) {
    return -1;
  }
  link->bytes_written += size;
  std::vector<uint8_t> response = link->device->receive(data, size);
  if (!response.empty()) {
    std::lock_guard<std::mutex> lock(link->mutex);
    link->pending.push_back(std::move(response));
    link->cv.notify_one();
  }
  return static_cast<int64_t>(size);
}

extern "C" void pd_test_link_close(pd_test_link* link) {
  if (link != nullptr) {
    ++link->closes;
  }
}

extern "C" size_t pd_test_link_connects(pd_test_link* link) {
  return link != nullptr ? link->connects.load() : 0;
}

extern "C" size_t pd_test_link_closes(pd_test_link* link) {
  return link != nullptr ? link->closes.load() : 0;
}

extern "C" size_t pd_test_link_bytes_written(pd_test_link* link) {
  return link != nullptr ? link->bytes_written.load() : 0;
}

extern "C" size_t pd_test_link_cuts(pd_test_link* link) {
  return link != nullptr ? link->device->cuts() : 0;
}

extern "C" int pd_test_link_received_contains(pd_test_link* link, const char* needle) {
  if (link == nullptr || needle == nullptr) {
    return 0;
  }
  return link->device->receivedContains(needle) ? 1 : 0;
}

// --- M15: the loopback listener ------------------------------------------------------

struct pd_test_listener {
  std::shared_ptr<pdfake::FakePrinter> device = std::make_shared<pdfake::FakePrinter>();
  std::unique_ptr<pdfake::FakePrinterServer> server;
};

extern "C" pd_test_listener* pd_test_listener_start(const char* script_id) {
  if (script_id == nullptr) {
    return nullptr;
  }
  const std::string id(script_id);
  pdfake::Script script;
  if (id == "ok") {
    script.answer_identity = true;  // "EPOSN" / "TM-T88V" — the impersonation case
  } else if (id == "silent") {
    script.answer_realtime = false;
    script.answer_identity = false;
    script.answer_process_id = false;
    script.answer_queued_status = false;
    script.answer_asb = false;
  } else {
    return nullptr;
  }

  auto listener = new pd_test_listener();
  listener->device->setScript(script);
  listener->server.reset(new pdfake::FakePrinterServer(listener->device));
  if (!listener->server->start()) {
    delete listener;
    return nullptr;
  }
  return listener;
}

extern "C" uint16_t pd_test_listener_port(pd_test_listener* listener) {
  return listener != nullptr && listener->server ? listener->server->port() : 0;
}

extern "C" size_t pd_test_listener_print_data_bytes(pd_test_listener* listener) {
  return listener != nullptr ? listener->device->printDataBytes() : 0;
}

extern "C" void pd_test_listener_stop(pd_test_listener* listener) {
  if (listener != nullptr && listener->server) {
    listener->server->stop();
  }
}

extern "C" void pd_test_listener_destroy(pd_test_listener* listener) {
  delete listener;
}

// --- Enum bridge ---------------------------------------------------------------------
//
// JobState, ConfidenceLevel, DeviceEvent, FailureReason, JobOutcome, ConfidenceGrade,
// CompletionAuthority and the four M14 drawer enums (cash_drawer.hpp) are counted from
// the core's own kAll* arrays: if a member is added there and pd.h is not updated to
// match, this count moves and pd.h's _COUNT does not. The other eight enums
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
    // M13b: + StarEtb and StarEscGsEtx (docs/wire-protocols.md §2).
    case PD_TEST_ENUM_COMPLETION: return 8;       // CompletionMechanism incl. vendor mechanisms
    case PD_TEST_ENUM_CUT_VARIANT: return 3;      // CutVariant: Partial, Full, None
    case PD_TEST_ENUM_ALIGNMENT: return 3;        // escpos::Alignment: Left, Center, Right
    case PD_TEST_ENUM_CODE_PAGE: return 5;        // escpos::CodePage, non-contiguous values
    case PD_TEST_ENUM_BINARIZATION: return 2;     // escpos::Binarization
    case PD_TEST_ENUM_CONFIDENCE_GRADE:
      return static_cast<int>(pd::kAllConfidenceGrades.size());
    case PD_TEST_ENUM_COMPLETION_AUTHORITY:
      return static_cast<int>(pd::kAllCompletionAuthorities.size());
    // M14. Counted from the core's own kAll* arrays, like the seven above: a member
    // added to cash_drawer.hpp without regenerating pd.h moves this and not the
    // header's _COUNT.
    case PD_TEST_ENUM_DRAWER_STATE: return static_cast<int>(pd::kAllDrawerStates.size());
    case PD_TEST_ENUM_DRAWER_PORT_STANDARD:
      return static_cast<int>(pd::kAllDrawerPortStandards.size());
    case PD_TEST_ENUM_DRAWER_KICK_METHOD:
      return static_cast<int>(pd::kAllDrawerKickMethods.size());
    case PD_TEST_ENUM_DRAWER_STATUS_METHOD:
      return static_cast<int>(pd::kAllDrawerStatusMethods.size());
    // M15 — docs/api.md §15, same rule again.
    case PD_TEST_ENUM_PROFILE_SELECTION:
      return static_cast<int>(pd::kAllProfileSelections.size());
    case PD_TEST_ENUM_DETECTION_STATUS:
      return static_cast<int>(pd::kAllDetectionStatuses.size());
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
      static constexpr pd::CompletionMechanism kValues[] = {
          pd::CompletionMechanism::GsParenH,  pd::CompletionMechanism::GsR1,
          pd::CompletionMechanism::VendorIdle, pd::CompletionMechanism::EposJobId,
          pd::CompletionMechanism::StarCheckedBlock, pd::CompletionMechanism::None,
          /* M13b */ pd::CompletionMechanism::StarEtb,
          pd::CompletionMechanism::StarEscGsEtx};
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
    case PD_TEST_ENUM_CONFIDENCE_GRADE:
      return static_cast<int>(pd::kAllConfidenceGrades[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_COMPLETION_AUTHORITY:
      return static_cast<int>(pd::kAllCompletionAuthorities[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_DRAWER_STATE:
      return static_cast<int>(pd::kAllDrawerStates[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_DRAWER_PORT_STANDARD:
      return static_cast<int>(pd::kAllDrawerPortStandards[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_DRAWER_KICK_METHOD:
      return static_cast<int>(pd::kAllDrawerKickMethods[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_DRAWER_STATUS_METHOD:
      return static_cast<int>(pd::kAllDrawerStatusMethods[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_PROFILE_SELECTION:
      return static_cast<int>(pd::kAllProfileSelections[static_cast<size_t>(index)]);
    case PD_TEST_ENUM_DETECTION_STATUS:
      return static_cast<int>(pd::kAllDetectionStatuses[static_cast<size_t>(index)]);
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
    case PD_TEST_ENUM_CONFIDENCE_GRADE:
      return pd::to_string(static_cast<pd::ConfidenceGrade>(value));
    case PD_TEST_ENUM_COMPLETION_AUTHORITY:
      return pd::to_string(static_cast<pd::CompletionAuthority>(value));
    case PD_TEST_ENUM_DRAWER_STATE:
      return pd::to_string(static_cast<pd::DrawerState>(value));
    case PD_TEST_ENUM_DRAWER_PORT_STANDARD:
      return pd::to_string(static_cast<pd::DrawerPortStandard>(value));
    case PD_TEST_ENUM_DRAWER_KICK_METHOD:
      return pd::to_string(static_cast<pd::DrawerKickMethod>(value));
    case PD_TEST_ENUM_DRAWER_STATUS_METHOD:
      return pd::to_string(static_cast<pd::DrawerStatusMethod>(value));
    case PD_TEST_ENUM_PROFILE_SELECTION:
      return pd::to_string(static_cast<pd::ProfileSelection>(value));
    case PD_TEST_ENUM_DETECTION_STATUS:
      return pd::to_string(static_cast<pd::DetectionStatus>(value));
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
    case PD_TEST_ENUM_CONFIDENCE_GRADE: return "ConfidenceGrade";
    case PD_TEST_ENUM_COMPLETION_AUTHORITY: return "CompletionAuthority";
    case PD_TEST_ENUM_DRAWER_STATE: return "DrawerState";
    case PD_TEST_ENUM_DRAWER_PORT_STANDARD: return "DrawerPortStandard";
    case PD_TEST_ENUM_DRAWER_KICK_METHOD: return "DrawerKickMethod";
    case PD_TEST_ENUM_DRAWER_STATUS_METHOD: return "DrawerStatusMethod";
    case PD_TEST_ENUM_PROFILE_SELECTION: return "ProfileSelection";
    case PD_TEST_ENUM_DETECTION_STATUS: return "DetectionStatus";
    case PD_TEST_ENUM_TOTAL: break;
  }
  return "UnknownEnum";
}
