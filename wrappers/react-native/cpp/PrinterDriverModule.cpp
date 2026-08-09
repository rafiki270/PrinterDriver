// The C++ TurboModule. See PrinterDriverModule.h for the threading contract; this file is
// the translation layer that implements it.
//
// The only SDK header included here is pd.h. Nothing under core/ or capi/ is touched
// directly: the C ABI is the contract this file binds, exactly as the JNI glue and the
// Swift wrapper bind it, so a change that keeps pd.h intact cannot break this module and a
// change that breaks pd.h breaks all four wrappers at once, visibly.

#ifndef PD_RN_TEST_STUB
#include <ReactCommon/CallInvoker.h>
#include <ReactCommon/TurboModule.h>
#include <jsi/jsi.h>
#else
// See cpp/__tests__/rn_stub.h. Host-side syntax checking only; the real builds never
// define PD_RN_TEST_STUB.
#endif

#include "PrinterDriverModule.h"

#include "printerdriver/pd.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdrn {
namespace {

namespace jsi = facebook::jsi;
using facebook::react::CallInvoker;

/// How long a core thread waits for JavaScript to answer a question before falling back to
/// the registration's documented failure. Generous, because the JS thread may be busy
/// rendering; finite, because a printer must never hang on a callback that never settles.
constexpr uint32_t kJsAnswerTimeoutMs = 5000;

// =========================================================================================
// jsi helpers
// =========================================================================================

/// Backing store for an ArrayBuffer handed to JavaScript. The bytes are copied out of the
/// core's transient buffer first: pd.h is explicit that a callback's data is valid only for
/// the duration of the call, and the JS thread runs later.
class ByteBuffer : public jsi::MutableBuffer {
 public:
  explicit ByteBuffer(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  size_t size() const override { return bytes_.size(); }
  uint8_t* data() override { return bytes_.data(); }

 private:
  std::vector<uint8_t> bytes_;
};

jsi::Value makeString(jsi::Runtime& rt, const char* text) {
  return jsi::Value(jsi::String::createFromUtf8(rt, text == nullptr ? "" : text));
}

jsi::Value makeString(jsi::Runtime& rt, const std::string& text) {
  return jsi::Value(jsi::String::createFromUtf8(rt, text));
}

jsi::Value makeNumber(double value) { return jsi::Value(value); }

jsi::Value makeBool(bool value) { return jsi::Value(value); }

jsi::Value makeBytes(jsi::Runtime& rt, const uint8_t* data, size_t size) {
  std::vector<uint8_t> copy;
  if (data != nullptr && size > 0) copy.assign(data, data + size);
  return jsi::Value(jsi::ArrayBuffer(rt, std::make_shared<ByteBuffer>(std::move(copy))));
}

std::string asString(jsi::Runtime& rt, const jsi::Value& value) {
  if (!value.isString()) return std::string();
  return value.getString(rt).utf8(rt);
}

int32_t asInt(const jsi::Value& value) {
  if (!value.isNumber()) return 0;
  return static_cast<int32_t>(value.getNumber());
}

uint32_t asUint(const jsi::Value& value) {
  if (!value.isNumber()) return 0u;
  const double number = value.getNumber();
  return number < 0.0 ? 0u : static_cast<uint32_t>(number);
}

bool asBool(const jsi::Value& value) { return value.isBool() && value.getBool(); }

std::string stringField(jsi::Runtime& rt, const jsi::Object& object, const char* name) {
  return asString(rt, object.getProperty(rt, name));
}

int32_t intField(jsi::Runtime& rt, const jsi::Object& object, const char* name) {
  return asInt(object.getProperty(rt, name));
}

uint32_t uintField(jsi::Runtime& rt, const jsi::Object& object, const char* name) {
  return asUint(object.getProperty(rt, name));
}

bool boolField(jsi::Runtime& rt, const jsi::Object& object, const char* name) {
  const jsi::Value value = object.getProperty(rt, name);
  if (value.isBool()) return value.getBool();
  return value.isNumber() && value.getNumber() != 0.0;
}

/// The bytes of an ArrayBuffer property, copied. Used for everything except the raster
/// pixel plane, which is read in place (see the `print` method).
std::vector<uint8_t> bytesField(jsi::Runtime& rt, const jsi::Object& object, const char* name) {
  std::vector<uint8_t> bytes;
  const jsi::Value value = object.getProperty(rt, name);
  if (!value.isObject()) return bytes;
  const jsi::Object holder = value.getObject(rt);
  if (!holder.isArrayBuffer(rt)) return bytes;
  const jsi::ArrayBuffer buffer = holder.getArrayBuffer(rt);
  const uint8_t* data = buffer.data(rt);
  const size_t size = buffer.size(rt);
  if (data != nullptr && size > 0) bytes.assign(data, data + size);
  return bytes;
}

std::vector<std::string> stringArrayField(jsi::Runtime& rt, const jsi::Object& object,
                                          const char* name) {
  std::vector<std::string> items;
  const jsi::Value value = object.getProperty(rt, name);
  if (!value.isObject()) return items;
  const jsi::Object holder = value.getObject(rt);
  if (!holder.isArray(rt)) return items;
  const jsi::Array array = holder.getArray(rt);
  const size_t length = array.size(rt);
  items.reserve(length);
  for (size_t index = 0; index < length; ++index) {
    items.push_back(asString(rt, array.getValueAtIndex(rt, index)));
  }
  return items;
}

// =========================================================================================
// Data copied out of the core's transient callback structs
// =========================================================================================

struct DetectionSummaryData {
  std::string endpoint;
  std::string vendor;
  std::string model;
  std::string firmware;
  std::string serial;
  int32_t identityTrusted = 0;
  int32_t confidencePercent = 0;
  int32_t impersonationSuspected = 0;
  int32_t identityFresh = 0;
  std::string profileId;
  int32_t selection = 0;
  int32_t nominalPaperMm = 0;
  int32_t printableWidthDots = 0;
  int32_t charsPerLine = 0;
  int32_t dpi = 0;
  int32_t completion = 0;
  int32_t gradeCeiling = 0;
  int32_t authority = 0;
  std::string method;
  int32_t completionProvenance = 0;
  int32_t drawerPresent = 0;
  int32_t drawerKickable = 0;
  int32_t drawerStandard = 0;
  int32_t drawerVoltage = 0;
  int32_t drawerElectricalProvenance = 0;
  int32_t drawerCommandsProvenance = 0;
  std::string provenanceSummary;
  std::vector<std::string> degradations;
};

std::string safe(const char* text) { return text == nullptr ? std::string() : std::string(text); }

DetectionSummaryData copySummary(const pd_detection_summary& summary) {
  DetectionSummaryData data;
  data.endpoint = safe(summary.endpoint);
  data.vendor = safe(summary.vendor);
  data.model = safe(summary.model);
  data.firmware = safe(summary.firmware);
  data.serial = safe(summary.serial);
  data.identityTrusted = summary.identity_trusted;
  data.confidencePercent = static_cast<int32_t>(summary.confidence_percent);
  data.impersonationSuspected = summary.impersonation_suspected;
  data.identityFresh = summary.identity_fresh;
  data.profileId = safe(summary.profile_id);
  data.selection = static_cast<int32_t>(summary.selection);
  data.nominalPaperMm = static_cast<int32_t>(summary.nominal_paper_mm);
  data.printableWidthDots = static_cast<int32_t>(summary.printable_width_dots);
  data.charsPerLine = static_cast<int32_t>(summary.chars_per_line);
  data.dpi = static_cast<int32_t>(summary.dpi);
  data.completion = static_cast<int32_t>(summary.completion);
  data.gradeCeiling = static_cast<int32_t>(summary.grade_ceiling);
  data.authority = static_cast<int32_t>(summary.authority);
  data.method = safe(summary.method);
  data.completionProvenance = static_cast<int32_t>(summary.completion_provenance);
  data.drawerPresent = summary.drawer_present;
  data.drawerKickable = summary.drawer_kickable;
  data.drawerStandard = static_cast<int32_t>(summary.drawer_standard);
  data.drawerVoltage = static_cast<int32_t>(summary.drawer_voltage);
  data.drawerElectricalProvenance = static_cast<int32_t>(summary.drawer_electrical_provenance);
  data.drawerCommandsProvenance = static_cast<int32_t>(summary.drawer_commands_provenance);
  data.provenanceSummary = safe(summary.provenance_summary);
  for (size_t index = 0; index < summary.degradation_count; ++index) {
    if (summary.degradations == nullptr) break;
    data.degradations.push_back(safe(summary.degradations[index]));
  }
  return data;
}

jsi::Value summaryToJs(jsi::Runtime& rt, const DetectionSummaryData& data) {
  jsi::Object object(rt);
  object.setProperty(rt, "endpoint", makeString(rt, data.endpoint));
  object.setProperty(rt, "vendor", makeString(rt, data.vendor));
  object.setProperty(rt, "model", makeString(rt, data.model));
  object.setProperty(rt, "firmware", makeString(rt, data.firmware));
  object.setProperty(rt, "serial", makeString(rt, data.serial));
  object.setProperty(rt, "identityTrusted", makeNumber(data.identityTrusted));
  object.setProperty(rt, "confidencePercent", makeNumber(data.confidencePercent));
  object.setProperty(rt, "impersonationSuspected", makeNumber(data.impersonationSuspected));
  object.setProperty(rt, "identityFresh", makeNumber(data.identityFresh));
  object.setProperty(rt, "profileId", makeString(rt, data.profileId));
  object.setProperty(rt, "selection", makeNumber(data.selection));
  object.setProperty(rt, "nominalPaperMm", makeNumber(data.nominalPaperMm));
  object.setProperty(rt, "printableWidthDots", makeNumber(data.printableWidthDots));
  object.setProperty(rt, "charsPerLine", makeNumber(data.charsPerLine));
  object.setProperty(rt, "dpi", makeNumber(data.dpi));
  object.setProperty(rt, "completion", makeNumber(data.completion));
  object.setProperty(rt, "gradeCeiling", makeNumber(data.gradeCeiling));
  object.setProperty(rt, "authority", makeNumber(data.authority));
  object.setProperty(rt, "method", makeString(rt, data.method));
  object.setProperty(rt, "completionProvenance", makeNumber(data.completionProvenance));
  object.setProperty(rt, "drawerPresent", makeNumber(data.drawerPresent));
  object.setProperty(rt, "drawerKickable", makeNumber(data.drawerKickable));
  object.setProperty(rt, "drawerStandard", makeNumber(data.drawerStandard));
  object.setProperty(rt, "drawerVoltage", makeNumber(data.drawerVoltage));
  object.setProperty(rt, "drawerElectricalProvenance",
                     makeNumber(data.drawerElectricalProvenance));
  object.setProperty(rt, "drawerCommandsProvenance", makeNumber(data.drawerCommandsProvenance));
  object.setProperty(rt, "provenanceSummary", makeString(rt, data.provenanceSummary));
  jsi::Array degradations(rt, data.degradations.size());
  for (size_t index = 0; index < data.degradations.size(); ++index) {
    degradations.setValueAtIndex(rt, index, makeString(rt, data.degradations[index]));
  }
  object.setProperty(rt, "degradations", jsi::Value(std::move(degradations)));
  return jsi::Value(std::move(object));
}

struct DetectedPrinterData {
  std::string endpoint;
  std::string host;
  int32_t port = 0;
  int32_t status = 0;
  int32_t portOpen = 0;
  int32_t fromCache = 0;
  std::string dleEotHex;
  DetectionSummaryData summary;
};

DetectedPrinterData copyDetected(const pd_detected_printer& printer) {
  DetectedPrinterData data;
  data.endpoint = safe(printer.endpoint);
  data.host = safe(printer.host);
  data.port = static_cast<int32_t>(printer.port);
  data.status = static_cast<int32_t>(printer.status);
  data.portOpen = printer.port_open;
  data.fromCache = printer.from_cache;
  data.dleEotHex = safe(printer.dle_eot_hex);
  data.summary = copySummary(printer.summary);
  return data;
}

jsi::Value detectedToJs(jsi::Runtime& rt, const DetectedPrinterData& data) {
  jsi::Object object(rt);
  object.setProperty(rt, "endpoint", makeString(rt, data.endpoint));
  object.setProperty(rt, "host", makeString(rt, data.host));
  object.setProperty(rt, "port", makeNumber(data.port));
  object.setProperty(rt, "status", makeNumber(data.status));
  object.setProperty(rt, "portOpen", makeNumber(data.portOpen));
  object.setProperty(rt, "fromCache", makeNumber(data.fromCache));
  object.setProperty(rt, "dleEotHex", makeString(rt, data.dleEotHex));
  object.setProperty(rt, "summary", summaryToJs(rt, data.summary));
  return jsi::Value(std::move(object));
}

struct DiscoveredDeviceData {
  std::string ip;
  int32_t port = 0;
  int32_t port9100Open = 0;
  std::string dleEotHex;
};

DiscoveredDeviceData copyDiscovered(const pd_discovered_device& device) {
  DiscoveredDeviceData data;
  data.ip = safe(device.ip);
  data.port = static_cast<int32_t>(device.port);
  data.port9100Open = device.port9100_open;
  data.dleEotHex = safe(device.dle_eot_hex);
  return data;
}

jsi::Value discoveredToJs(jsi::Runtime& rt, const DiscoveredDeviceData& data) {
  jsi::Object object(rt);
  object.setProperty(rt, "ip", makeString(rt, data.ip));
  object.setProperty(rt, "port", makeNumber(data.port));
  object.setProperty(rt, "port9100Open", makeNumber(data.port9100Open));
  object.setProperty(rt, "dleEotHex", makeString(rt, data.dleEotHex));
  return jsi::Value(std::move(object));
}

jsi::Value statusToJs(jsi::Runtime& rt, const pd_device_status& status) {
  jsi::Object object(rt);
  object.setProperty(rt, "connected", makeNumber(status.connected));
  object.setProperty(rt, "observed", makeNumber(status.observed));
  object.setProperty(rt, "online", makeNumber(status.online));
  object.setProperty(rt, "coverOpen", makeNumber(status.cover_open));
  object.setProperty(rt, "paperOut", makeNumber(status.paper_out));
  object.setProperty(rt, "paperNearEnd", makeNumber(status.paper_near_end));
  object.setProperty(rt, "cutterError", makeNumber(status.cutter_error));
  object.setProperty(rt, "unrecoverableError", makeNumber(status.unrecoverable_error));
  object.setProperty(rt, "recoverableError", makeNumber(status.recoverable_error));
  return jsi::Value(std::move(object));
}

jsi::Value resultToJs(jsi::Runtime& rt, const pd_job_result& result, const std::string& method) {
  jsi::Object object(rt);
  object.setProperty(rt, "outcome", makeNumber(static_cast<int32_t>(result.outcome)));
  object.setProperty(rt, "confidence", makeNumber(static_cast<int32_t>(result.confidence)));
  object.setProperty(rt, "reason", makeNumber(static_cast<int32_t>(result.reason)));
  object.setProperty(rt, "grade", makeNumber(static_cast<int32_t>(result.grade)));
  object.setProperty(rt, "authority", makeNumber(static_cast<int32_t>(result.authority)));
  object.setProperty(rt, "method", makeString(rt, method));
  return jsi::Value(std::move(object));
}

jsi::Value drawerCapabilitiesToJs(jsi::Runtime& rt, const pd_drawer_capabilities& caps) {
  jsi::Object object(rt);
  object.setProperty(rt, "present", makeNumber(caps.present));
  object.setProperty(rt, "standard", makeNumber(static_cast<int32_t>(caps.standard)));
  object.setProperty(rt, "voltage", makeNumber(caps.voltage));
  object.setProperty(rt, "maxCurrentMa", makeNumber(caps.max_current_ma));
  object.setProperty(rt, "channelCount", makeNumber(caps.channel_count));
  object.setProperty(rt, "sensorPin", makeNumber(caps.sensor_pin));
  object.setProperty(rt, "method", makeNumber(static_cast<int32_t>(caps.method)));
  object.setProperty(rt, "defaultPulseMs", makeNumber(caps.default_pulse_ms));
  object.setProperty(rt, "maxPulseMs", makeNumber(caps.max_pulse_ms));
  object.setProperty(rt, "cooldownMs", makeNumber(caps.cooldown_ms));
  object.setProperty(rt, "canKickDuringPrint", makeNumber(caps.can_kick_during_print));
  object.setProperty(rt, "statusAvailable", makeNumber(caps.status_available));
  object.setProperty(rt, "statusMethod", makeNumber(static_cast<int32_t>(caps.status_method)));
  object.setProperty(rt, "sharedBetweenDrawers", makeNumber(caps.shared_between_drawers));
  object.setProperty(rt, "sharedWithBuzzer", makeNumber(caps.shared_with_buzzer));
  object.setProperty(rt, "electricalProvenance",
                     makeNumber(static_cast<int32_t>(caps.electrical_provenance)));
  object.setProperty(rt, "commandsProvenance",
                     makeNumber(static_cast<int32_t>(caps.commands_provenance)));
  object.setProperty(rt, "kickable", makeNumber(caps.kickable));
  return jsi::Value(std::move(object));
}

jsi::Value drawerResultToJs(jsi::Runtime& rt, const pd_drawer_result& result) {
  jsi::Object object(rt);
  object.setProperty(rt, "state", makeNumber(static_cast<int32_t>(result.state)));
  object.setProperty(rt, "previousState", makeNumber(static_cast<int32_t>(result.previous_state)));
  object.setProperty(rt, "channel", makeNumber(result.channel));
  object.setProperty(rt, "pulseMs", makeNumber(result.pulse_ms));
  object.setProperty(rt, "elapsedMs", makeNumber(static_cast<double>(result.elapsed_ms)));
  return jsi::Value(std::move(object));
}

jsi::Value drawerReadingToJs(jsi::Runtime& rt, const pd_drawer_reading& reading) {
  jsi::Object object(rt);
  object.setProperty(rt, "available", makeNumber(reading.available));
  object.setProperty(rt, "answered", makeNumber(reading.answered));
  object.setProperty(rt, "pinHigh", makeNumber(reading.pin_high));
  object.setProperty(rt, "needsCalibration", makeNumber(reading.needs_calibration));
  object.setProperty(rt, "state", makeNumber(static_cast<int32_t>(reading.state)));
  return jsi::Value(std::move(object));
}

// --- M19: the render report -------------------------------------------------------------
//
// pd.h is explicit that pd_render_result::bytes and every string reachable through
// pd_render_report_at belong to the DRIVER and stay valid only until the next render on it.
// Both renders below run on a worker thread and resolve a Promise, so the JS thread always
// runs LATER -- possibly after another render has already replaced those strings. The whole
// report is therefore copied out on the thread that produced it, exactly as the detection
// structs are, and the index reader is never called from the JS side of the boundary.

struct ReportEntryData {
  int32_t kind = 0;
  std::string block;
  std::string requested;
  std::string delivered;
  int32_t path = 0;
  std::string note;
};

ReportEntryData copyReportEntry(const pd_report_entry& entry) {
  ReportEntryData data;
  data.kind = static_cast<int32_t>(entry.kind);
  data.block = safe(entry.block);
  data.requested = safe(entry.requested);
  data.delivered = safe(entry.delivered);
  data.path = static_cast<int32_t>(entry.path);
  data.note = safe(entry.note);
  return data;
}

/// The last render's report on `driver`, pulled out through the ABI's indexed reader.
std::vector<ReportEntryData> readRenderReport(pd_driver* driver) {
  std::vector<ReportEntryData> entries;
  if (driver == nullptr) return entries;
  const size_t count = pd_render_report_count(driver);
  entries.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    pd_report_entry wire{};
    if (pd_render_report_at(driver, static_cast<int32_t>(index), &wire) != 1) continue;
    entries.push_back(copyReportEntry(wire));
  }
  return entries;
}

jsi::Value reportEntryToJs(jsi::Runtime& rt, const ReportEntryData& data) {
  jsi::Object object(rt);
  object.setProperty(rt, "kind", makeNumber(data.kind));
  object.setProperty(rt, "block", makeString(rt, data.block));
  object.setProperty(rt, "requested", makeString(rt, data.requested));
  object.setProperty(rt, "delivered", makeString(rt, data.delivered));
  object.setProperty(rt, "path", makeNumber(data.path));
  object.setProperty(rt, "note", makeString(rt, data.note));
  return jsi::Value(std::move(object));
}

jsi::Value reportToJs(jsi::Runtime& rt, const std::vector<ReportEntryData>& entries) {
  jsi::Array array(rt, entries.size());
  for (size_t index = 0; index < entries.size(); ++index) {
    array.setValueAtIndex(rt, index, reportEntryToJs(rt, entries[index]));
  }
  return jsi::Value(std::move(array));
}

// =========================================================================================
// Module state
// =========================================================================================

/// One question in flight from a core thread to JavaScript.
struct PendingRequest {
  std::mutex mutex;
  std::condition_variable cv;
  bool answered = false;
  bool ok = false;
  double number = 0.0;
  std::string text;
  std::vector<uint8_t> bytes;
};

struct JsTransport;
struct JsRegistration;

}  // namespace

/// Declared in PrinterDriverModule.h and defined here, in the enclosing namespace rather
/// than the anonymous one, so the two declarations name the same type.
struct ModuleState : std::enable_shared_from_this<ModuleState> {
  std::shared_ptr<CallInvoker> jsInvoker;

  /// Set the first time `get` runs and cleared by the destructor. Every cross-thread post
  /// goes through the CallInvoker, so this is only ever dereferenced on the JS thread.
  jsi::Runtime* runtime = nullptr;
  std::shared_ptr<jsi::Function> sink;

  std::mutex handles;
  int32_t nextHandle = 1;
  std::unordered_map<int32_t, pd_driver*> drivers;
  std::unordered_map<int32_t, pd_printer*> printers;
  std::unordered_map<const void*, int32_t> printerTokens;
  std::unordered_map<int32_t, pd_job*> jobs;
  std::unordered_map<const void*, int32_t> jobTokens;
  std::unordered_map<int32_t, pd_queue*> queues;

  std::mutex requests;
  int32_t nextRequestId = 1;
  std::unordered_map<int32_t, std::shared_ptr<PendingRequest>> pending;

  /// Callback contexts pd.h requires to stay alive until pd_destroy.
  std::vector<std::unique_ptr<JsTransport>> transports;
  std::vector<std::unique_ptr<JsRegistration>> registrations;
  std::vector<std::unique_ptr<std::string>> descriptions;
  std::vector<std::unique_ptr<std::vector<uint8_t>>> probeRequests;
};

namespace {

struct JsTransport {
  std::weak_ptr<ModuleState> state;
  int32_t id = 0;
};

struct JsRegistration {
  std::weak_ptr<ModuleState> state;
  std::string id;
  bool hasStatus = false;
};

int32_t mintHandle(ModuleState& state) {
  const int32_t handle = state.nextHandle;
  state.nextHandle += 1;
  return handle;
}

int32_t registerDriver(ModuleState& state, pd_driver* driver) {
  if (driver == nullptr) return 0;
  std::lock_guard<std::mutex> guard(state.handles);
  const int32_t handle = mintHandle(state);
  state.drivers[handle] = driver;
  return handle;
}

/// The same pd_printer* always gets the same token, which is what keeps pd.h's
/// "the same underlying job always maps to the same pd_job pointer" visible in JavaScript.
int32_t tokenForPrinter(ModuleState& state, pd_printer* printer) {
  if (printer == nullptr) return 0;
  std::lock_guard<std::mutex> guard(state.handles);
  const auto existing = state.printerTokens.find(printer);
  if (existing != state.printerTokens.end()) return existing->second;
  const int32_t handle = mintHandle(state);
  state.printers[handle] = printer;
  state.printerTokens[printer] = handle;
  return handle;
}

int32_t tokenForJob(ModuleState& state, pd_job* job) {
  if (job == nullptr) return 0;
  std::lock_guard<std::mutex> guard(state.handles);
  const auto existing = state.jobTokens.find(job);
  if (existing != state.jobTokens.end()) return existing->second;
  const int32_t handle = mintHandle(state);
  state.jobs[handle] = job;
  state.jobTokens[job] = handle;
  return handle;
}

int32_t registerQueue(ModuleState& state, pd_queue* queue) {
  if (queue == nullptr) return 0;
  std::lock_guard<std::mutex> guard(state.handles);
  const int32_t handle = mintHandle(state);
  state.queues[handle] = queue;
  return handle;
}

pd_driver* driverAt(ModuleState& state, int32_t handle) {
  std::lock_guard<std::mutex> guard(state.handles);
  const auto found = state.drivers.find(handle);
  return found == state.drivers.end() ? nullptr : found->second;
}

pd_printer* printerAt(ModuleState& state, int32_t handle) {
  std::lock_guard<std::mutex> guard(state.handles);
  const auto found = state.printers.find(handle);
  return found == state.printers.end() ? nullptr : found->second;
}

pd_job* jobAt(ModuleState& state, int32_t handle) {
  std::lock_guard<std::mutex> guard(state.handles);
  const auto found = state.jobs.find(handle);
  return found == state.jobs.end() ? nullptr : found->second;
}

pd_queue* queueAt(ModuleState& state, int32_t handle) {
  std::lock_guard<std::mutex> guard(state.handles);
  const auto found = state.queues.find(handle);
  return found == state.queues.end() ? nullptr : found->second;
}

pd_driver* requireDriver(ModuleState& state, jsi::Runtime& rt, const jsi::Value& value) {
  pd_driver* driver = driverAt(state, asInt(value));
  if (driver == nullptr) throw jsi::JSError(rt, "printerdriver: unknown driver handle");
  return driver;
}

pd_printer* requirePrinter(ModuleState& state, jsi::Runtime& rt, const jsi::Value& value) {
  pd_printer* printer = printerAt(state, asInt(value));
  if (printer == nullptr) throw jsi::JSError(rt, "printerdriver: unknown printer handle");
  return printer;
}

pd_job* requireJob(ModuleState& state, jsi::Runtime& rt, const jsi::Value& value) {
  pd_job* job = jobAt(state, asInt(value));
  if (job == nullptr) throw jsi::JSError(rt, "printerdriver: unknown job handle");
  return job;
}

pd_queue* requireQueue(ModuleState& state, jsi::Runtime& rt, const jsi::Value& value) {
  pd_queue* queue = queueAt(state, asInt(value));
  if (queue == nullptr) throw jsi::JSError(rt, "printerdriver: unknown queue handle");
  return queue;
}

// =========================================================================================
// native -> JS notifications
// =========================================================================================

using PayloadFiller = std::function<void(jsi::Runtime&, jsi::Object&)>;

void postToSink(const std::shared_ptr<ModuleState>& state, std::string kind, PayloadFiller fill) {
  if (!state || !state->jsInvoker) return;
  std::weak_ptr<ModuleState> weak = state;
  state->jsInvoker->invokeAsync([weak, kind = std::move(kind), fill = std::move(fill)]() {
    const std::shared_ptr<ModuleState> live = weak.lock();
    if (!live || live->runtime == nullptr || !live->sink) return;
    jsi::Runtime& rt = *live->runtime;
    jsi::Object payload(rt);
    fill(rt, payload);
    live->sink->call(rt, jsi::String::createFromUtf8(rt, kind), std::move(payload));
  });
}

// =========================================================================================
// native -> JS -> native questions
// =========================================================================================

struct Answer {
  bool answered = false;
  bool ok = false;
  double number = 0.0;
  std::string text;
  std::vector<uint8_t> bytes;
};

/// Post a question and block this (core) thread until JavaScript answers or the deadline
/// passes. Never called on the JS thread -- see PrinterDriverModule.h.
Answer ask(const std::shared_ptr<ModuleState>& state, const char* kind, PayloadFiller fill) {
  Answer answer;
  if (!state || !state->jsInvoker) return answer;

  auto pending = std::make_shared<PendingRequest>();
  int32_t requestId = 0;
  {
    std::lock_guard<std::mutex> guard(state->requests);
    requestId = state->nextRequestId;
    state->nextRequestId += 1;
    state->pending[requestId] = pending;
  }

  postToSink(state, kind, [requestId, fill](jsi::Runtime& rt, jsi::Object& payload) {
    payload.setProperty(rt, "requestId", makeNumber(requestId));
    fill(rt, payload);
  });

  {
    std::unique_lock<std::mutex> lock(pending->mutex);
    pending->cv.wait_for(lock, std::chrono::milliseconds(kJsAnswerTimeoutMs),
                         [&pending]() { return pending->answered; });
    answer.answered = pending->answered;
    answer.ok = pending->ok;
    answer.number = pending->number;
    answer.text = pending->text;
    answer.bytes = pending->bytes;
  }

  {
    std::lock_guard<std::mutex> guard(state->requests);
    state->pending.erase(requestId);
  }
  return answer;
}

// =========================================================================================
// core callbacks
// =========================================================================================

void onLog(const char* message, void* ctx) {
  auto* raw = static_cast<ModuleState*>(ctx);
  if (raw == nullptr) return;
  const std::shared_ptr<ModuleState> state = raw->shared_from_this();
  const std::string copied = safe(message);
  postToSink(state, "log", [copied](jsi::Runtime& rt, jsi::Object& payload) {
    payload.setProperty(rt, "message", makeString(rt, copied));
  });
}

void onJobEvent(pd_job* job, pd_job_event event, void* ctx) {
  auto* raw = static_cast<ModuleState*>(ctx);
  if (raw == nullptr) return;
  const std::shared_ptr<ModuleState> state = raw->shared_from_this();
  const int32_t token = tokenForJob(*state, job);
  postToSink(state, "job", [token, event](jsi::Runtime& rt, jsi::Object& payload) {
    payload.setProperty(rt, "job", makeNumber(token));
    payload.setProperty(rt, "state", makeNumber(static_cast<int32_t>(event.state)));
    payload.setProperty(rt, "confidence", makeNumber(static_cast<int32_t>(event.confidence)));
    payload.setProperty(rt, "hasReason", makeNumber(event.has_reason));
    payload.setProperty(rt, "reason", makeNumber(static_cast<int32_t>(event.reason)));
    payload.setProperty(rt, "monotonicMs", makeNumber(static_cast<double>(event.monotonic_ms)));
  });
}

void onDeviceEvent(pd_printer* printer, pd_device_event event, void* ctx) {
  auto* raw = static_cast<ModuleState*>(ctx);
  if (raw == nullptr) return;
  const std::shared_ptr<ModuleState> state = raw->shared_from_this();
  const int32_t token = tokenForPrinter(*state, printer);
  postToSink(state, "device", [token, event](jsi::Runtime& rt, jsi::Object& payload) {
    payload.setProperty(rt, "printer", makeNumber(token));
    payload.setProperty(rt, "event", makeNumber(static_cast<int32_t>(event)));
  });
}

/// pd_auto_detect / pd_discover stream their candidates from worker threads while the
/// blocking call is still running on ours. The context carries the JS-side stream id.
struct SweepContext {
  std::shared_ptr<ModuleState> state;
  int32_t requestId = 0;
};

void onDetected(const pd_detected_printer* printer, uint64_t completed, uint64_t total,
                void* ctx) {
  auto* sweep = static_cast<SweepContext*>(ctx);
  if (sweep == nullptr || printer == nullptr) return;
  const DetectedPrinterData data = copyDetected(*printer);
  const int32_t requestId = sweep->requestId;
  postToSink(sweep->state, "detected",
             [data, requestId, completed, total](jsi::Runtime& rt, jsi::Object& payload) {
               payload.setProperty(rt, "requestId", makeNumber(requestId));
               payload.setProperty(rt, "printer", detectedToJs(rt, data));
               payload.setProperty(rt, "completed", makeNumber(static_cast<double>(completed)));
               payload.setProperty(rt, "total", makeNumber(static_cast<double>(total)));
             });
}

void onDiscovered(const pd_discovered_device* device, uint64_t completed, uint64_t total,
                  void* ctx) {
  auto* sweep = static_cast<SweepContext*>(ctx);
  if (sweep == nullptr || device == nullptr) return;
  const DiscoveredDeviceData data = copyDiscovered(*device);
  const int32_t requestId = sweep->requestId;
  postToSink(sweep->state, "discovered",
             [data, requestId, completed, total](jsi::Runtime& rt, jsi::Object& payload) {
               payload.setProperty(rt, "requestId", makeNumber(requestId));
               payload.setProperty(rt, "device", discoveredToJs(rt, data));
               payload.setProperty(rt, "completed", makeNumber(static_cast<double>(completed)));
               payload.setProperty(rt, "total", makeNumber(static_cast<double>(total)));
             });
}

// --- JS-supplied transports ---------------------------------------------------------------

int32_t transportConnect(void* ctx) {
  auto* transport = static_cast<JsTransport*>(ctx);
  if (transport == nullptr) return 0;
  const std::shared_ptr<ModuleState> state = transport->state.lock();
  const int32_t id = transport->id;
  const Answer answer = ask(state, "transport.connect",
                            [id](jsi::Runtime& rt, jsi::Object& payload) {
                              payload.setProperty(rt, "transport", makeNumber(id));
                            });
  // No answer means no link. Reporting a connection that was never made would let a job
  // claim transportAccepted for bytes nobody sent.
  return answer.answered && answer.ok ? 1 : 0;
}

int64_t transportWrite(void* ctx, const uint8_t* data, size_t size) {
  auto* transport = static_cast<JsTransport*>(ctx);
  if (transport == nullptr) return -1;
  const std::shared_ptr<ModuleState> state = transport->state.lock();
  const int32_t id = transport->id;
  std::vector<uint8_t> copy;
  if (data != nullptr && size > 0) copy.assign(data, data + size);
  const Answer answer = ask(state, "transport.write",
                            [id, copy](jsi::Runtime& rt, jsi::Object& payload) {
                              payload.setProperty(rt, "transport", makeNumber(id));
                              payload.setProperty(rt, "data",
                                                  makeBytes(rt, copy.data(), copy.size()));
                            });
  // A short write is reported honestly rather than rounded up: zero bytes out is a known
  // failure and one byte out is Unknown, and that difference decides whether an operator
  // should reprint (pd.h, pd_transport_write_fn).
  if (!answer.answered) return -1;
  return static_cast<int64_t>(answer.number);
}

void transportClose(void* ctx) {
  auto* transport = static_cast<JsTransport*>(ctx);
  if (transport == nullptr) return;
  const std::shared_ptr<ModuleState> state = transport->state.lock();
  const int32_t id = transport->id;
  ask(state, "transport.close", [id](jsi::Runtime& rt, jsi::Object& payload) {
    payload.setProperty(rt, "transport", makeNumber(id));
  });
}

// --- JS-supplied registrations ----------------------------------------------------------------

size_t fenceBytesThunk(void* ctx, const char* job_token, uint8_t* out, size_t cap) {
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return 0;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  const std::string token = safe(job_token);
  const Answer answer =
      ask(state, "completion.fenceBytes", [id, token, cap](jsi::Runtime& rt, jsi::Object& payload) {
        payload.setProperty(rt, "id", makeString(rt, id));
        payload.setProperty(rt, "jobToken", makeString(rt, token));
        payload.setProperty(rt, "cap", makeNumber(static_cast<double>(cap)));
      });
  if (!answer.answered) return 0;
  if (answer.bytes.size() > cap) {
    // Deliberately over-report rather than truncate: pd.h treats a fence longer than `cap`
    // as a registration it cannot honour and fails the job Unknown, which is right. A
    // truncated fence would be a fence the printer can never echo.
    return answer.bytes.size();
  }
  if (out != nullptr && !answer.bytes.empty()) {
    std::memcpy(out, answer.bytes.data(), answer.bytes.size());
  }
  return answer.bytes.size();
}

pd_match_result matcherThunk(void* ctx, const uint8_t* data, size_t size) {
  pd_match_result result;
  result.kind = PD_MATCH_NOT_MINE;
  result.token[0] = '\0';
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return result;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  std::vector<uint8_t> copy;
  if (data != nullptr && size > 0) copy.assign(data, data + size);
  const Answer answer =
      ask(state, "completion.match", [id, copy](jsi::Runtime& rt, jsi::Object& payload) {
        payload.setProperty(rt, "id", makeString(rt, id));
        payload.setProperty(rt, "data", makeBytes(rt, copy.data(), copy.size()));
      });
  if (!answer.answered) return result;
  const int32_t kind = static_cast<int32_t>(answer.number);
  if (kind < 0 || kind >= PD_MATCH_KIND_COUNT) return result;
  result.kind = static_cast<pd_match_kind>(kind);
  if (result.kind == PD_MATCH_MATCHED) {
    const size_t length = answer.text.size() < sizeof(result.token) - 1
                              ? answer.text.size()
                              : sizeof(result.token) - 1;
    if (length > 0) std::memcpy(result.token, answer.text.data(), length);
    result.token[length] = '\0';
  }
  return result;
}

pd_probe_finding classifyThunk(void* ctx, const uint8_t* response, size_t size) {
  pd_probe_finding finding;
  finding.answered = 0;
  finding.label[0] = '\0';
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return finding;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  std::vector<uint8_t> copy;
  if (response != nullptr && size > 0) copy.assign(response, response + size);
  const Answer answer =
      ask(state, "probe.classify", [id, copy](jsi::Runtime& rt, jsi::Object& payload) {
        payload.setProperty(rt, "id", makeString(rt, id));
        payload.setProperty(rt, "response", makeBytes(rt, copy.data(), copy.size()));
      });
  if (!answer.answered) return finding;
  finding.answered = answer.ok ? 1 : 0;
  const size_t length = answer.text.size() < sizeof(finding.label) - 1
                            ? answer.text.size()
                            : sizeof(finding.label) - 1;
  if (length > 0) std::memcpy(finding.label, answer.text.data(), length);
  finding.label[length] = '\0';
  return finding;
}

size_t blockHandlerThunk(void* ctx, const char* block_json, const char* profile_json,
                         uint8_t* out, size_t cap, int32_t* ok, char* detail,
                         size_t detail_cap) {
  auto* registration = static_cast<JsRegistration*>(ctx);
  const auto degrade = [&](const char* reason) -> size_t {
    if (ok != nullptr) *ok = 0;
    if (detail != nullptr && detail_cap > 0) {
      const size_t length = std::strlen(reason) < detail_cap - 1 ? std::strlen(reason)
                                                                 : detail_cap - 1;
      std::memcpy(detail, reason, length);
      detail[length] = '\0';
    }
    return 0;
  };
  if (registration == nullptr) return degrade("the block handler is gone");
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  const std::string block = safe(block_json);
  const std::string profile = safe(profile_json);
  const Answer answer = ask(
      state, "block.render", [id, block, profile, cap](jsi::Runtime& rt, jsi::Object& payload) {
        payload.setProperty(rt, "kind", makeString(rt, id));
        payload.setProperty(rt, "blockJson", makeString(rt, block));
        payload.setProperty(rt, "profileJson", makeString(rt, profile));
        payload.setProperty(rt, "cap", makeNumber(static_cast<double>(cap)));
      });
  if (!answer.answered) return degrade("the JavaScript block handler did not answer");
  if (!answer.ok) return degrade(answer.text.empty() ? "declined" : answer.text.c_str());
  if (answer.bytes.size() > cap) return degrade("the rendered block is larger than the buffer");
  if (out != nullptr && !answer.bytes.empty()) {
    std::memcpy(out, answer.bytes.data(), answer.bytes.size());
  }
  if (ok != nullptr) *ok = 1;
  return answer.bytes.size();
}

size_t formatterThunk(void* ctx, const char* value, const char* args, const char* locale,
                      char* out, size_t cap, int32_t* handled) {
  if (handled != nullptr) *handled = 0;
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return 0;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  const std::string valueText = safe(value);
  const std::string argsText = safe(args);
  const std::string localeText = safe(locale);
  const Answer answer =
      ask(state, "formatter.format",
          [id, valueText, argsText, localeText, cap](jsi::Runtime& rt, jsi::Object& payload) {
            payload.setProperty(rt, "name", makeString(rt, id));
            payload.setProperty(rt, "value", makeString(rt, valueText));
            payload.setProperty(rt, "args", makeString(rt, argsText));
            payload.setProperty(rt, "locale", makeString(rt, localeText));
            payload.setProperty(rt, "cap", makeNumber(static_cast<double>(cap)));
          });
  // Declining is the safe answer: the built-in formatter table is checked next.
  if (!answer.answered || !answer.ok) return 0;
  if (answer.text.size() > cap) return 0;
  if (out != nullptr && !answer.text.empty()) {
    std::memcpy(out, answer.text.data(), answer.text.size());
  }
  if (handled != nullptr) *handled = 1;
  return answer.text.size();
}

size_t drawerKickBytesThunk(void* ctx, uint8_t channel, uint16_t pulse_ms, uint8_t* out,
                            size_t cap) {
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return 0;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  const Answer answer =
      ask(state, "drawer.kickBytes",
          [id, channel, pulse_ms, cap](jsi::Runtime& rt, jsi::Object& payload) {
            payload.setProperty(rt, "id", makeString(rt, id));
            payload.setProperty(rt, "channel", makeNumber(channel));
            payload.setProperty(rt, "pulseMs", makeNumber(pulse_ms));
            payload.setProperty(rt, "cap", makeNumber(static_cast<double>(cap)));
          });
  if (!answer.answered || answer.bytes.size() > cap) return 0;
  if (out != nullptr && !answer.bytes.empty()) {
    std::memcpy(out, answer.bytes.data(), answer.bytes.size());
  }
  return answer.bytes.size();
}

size_t drawerStatusRequestThunk(void* ctx, uint8_t* out, size_t cap) {
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return 0;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  const Answer answer = ask(state, "drawer.statusRequest",
                            [id, cap](jsi::Runtime& rt, jsi::Object& payload) {
                              payload.setProperty(rt, "id", makeString(rt, id));
                              payload.setProperty(rt, "cap",
                                                  makeNumber(static_cast<double>(cap)));
                            });
  if (!answer.answered || answer.bytes.size() > cap) return 0;
  if (out != nullptr && !answer.bytes.empty()) {
    std::memcpy(out, answer.bytes.data(), answer.bytes.size());
  }
  return answer.bytes.size();
}

int32_t drawerStatusParseThunk(void* ctx, const uint8_t* response, size_t size) {
  auto* registration = static_cast<JsRegistration*>(ctx);
  if (registration == nullptr) return PD_UNKNOWN;
  const std::shared_ptr<ModuleState> state = registration->state.lock();
  const std::string id = registration->id;
  std::vector<uint8_t> copy;
  if (response != nullptr && size > 0) copy.assign(response, response + size);
  const Answer answer =
      ask(state, "drawer.statusParse", [id, copy](jsi::Runtime& rt, jsi::Object& payload) {
        payload.setProperty(rt, "id", makeString(rt, id));
        payload.setProperty(rt, "response", makeBytes(rt, copy.data(), copy.size()));
      });
  // A level nobody could interpret is PD_UNKNOWN, never a guessed direction.
  if (!answer.answered) return PD_UNKNOWN;
  const int32_t level = static_cast<int32_t>(answer.number);
  if (level == PD_TRUE || level == PD_FALSE) return level;
  return PD_UNKNOWN;
}

// =========================================================================================
// options and payloads, JS -> C
// =========================================================================================

pd_job_options readJobOptions(jsi::Runtime& rt, const jsi::Value& value) {
  pd_job_options options{};
  if (!value.isObject()) return options;
  const jsi::Object object = value.getObject(rt);
  // The key string is copied by pd.h before the call returns, so a temporary is fine --
  // but it must outlive the pd_* call, which is why the caller keeps `keyStorage`.
  options.cut = static_cast<pd_cut>(intField(rt, object, "cut"));
  options.open_drawer = intField(rt, object, "openDrawer");
  options.preflight = static_cast<pd_preflight>(intField(rt, object, "preflight"));
  options.timeout_ms = uintField(rt, object, "timeoutMs");
  options.top_feed_dots = uintField(rt, object, "topFeedDots");
  options.bottom_feed_dots = uintField(rt, object, "bottomFeedDots");
  options.suppress_verification_id = intField(rt, object, "suppressVerificationId");
  return options;
}

/**
 * pd_job_options plus the storage its `key` points at.
 *
 * The synchronous submit paths keep the key in a local because the pd_* call happens in the
 * same scope. pd_print_document_json does not: it renders first, on a worker thread, so the
 * options and their strings have to outlive this function. `rebind` is called there, after
 * the copy, because copying the struct moves the std::string and invalidates any c_str()
 * taken before it.
 */
struct JobOptionsStorage {
  pd_job_options options{};
  std::string key;

  void rebind() { options.key = key.empty() ? nullptr : key.c_str(); }
};

/// pd_render_options plus the three optional override strings it points at. Same rule.
struct RenderOptionsStorage {
  pd_render_options options{};
  std::string locale;
  std::string currency;
  std::string tz;

  void rebind() {
    // pd.h: NULL or "" defers to the document's own `meta`, so an omitted override stays
    // omitted rather than becoming an empty locale that overrides it with nothing.
    options.locale = locale.empty() ? nullptr : locale.c_str();
    options.currency = currency.empty() ? nullptr : currency.c_str();
    options.tz = tz.empty() ? nullptr : tz.c_str();
  }
};

pd_queue_options readQueueOptions(jsi::Runtime& rt, const jsi::Value& value) {
  pd_queue_options options{};
  if (!value.isObject()) return options;
  const jsi::Object object = value.getObject(rt);
  options.ttl_ms = uintField(rt, object, "ttlMs");
  options.priority = intField(rt, object, "priority");
  options.cut = static_cast<pd_cut>(intField(rt, object, "cut"));
  options.open_drawer = intField(rt, object, "openDrawer");
  options.preflight = static_cast<pd_preflight>(intField(rt, object, "preflight"));
  options.timeout_ms = uintField(rt, object, "timeoutMs");
  return options;
}

JobOptionsStorage readJobOptionsStorage(jsi::Runtime& rt, const jsi::Value& value) {
  JobOptionsStorage storage;
  storage.options = readJobOptions(rt, value);
  if (value.isObject()) storage.key = stringField(rt, value.getObject(rt), "key");
  return storage;
}

RenderOptionsStorage readRenderOptions(jsi::Runtime& rt, const jsi::Value& value) {
  RenderOptionsStorage storage;
  if (!value.isObject()) return storage;
  const jsi::Object object = value.getObject(rt);
  storage.options.width_dots = uintField(rt, object, "widthDots");
  storage.options.cut_clearance_dots =
      static_cast<uint16_t>(uintField(rt, object, "cutClearanceDots"));
  storage.options.max_rows_per_band = uintField(rt, object, "maxRowsPerBand");
  storage.locale = stringField(rt, object, "locale");
  storage.currency = stringField(rt, object, "currency");
  storage.tz = stringField(rt, object, "timeZone");
  return storage;
}

/// Everything a pd_payload points at, kept alive for the duration of the pd_* call.
struct PayloadStorage {
  pd_payload payload{};
  std::vector<pd_op> ops;
  std::vector<std::string> texts;
  std::vector<uint8_t> raw;
  /// Set for the raster tier: the JS ArrayBuffer's own memory, read in place. pd_print
  /// copies what it needs before returning, so nothing is retained past the call -- this is
  /// the zero-copy path docs/platforms.md asks for, and the reason base64 is not offered.
  const uint8_t* pixels = nullptr;
};

void readPayload(jsi::Runtime& rt, const jsi::Value& value, PayloadStorage& storage) {
  if (!value.isObject()) throw jsi::JSError(rt, "printerdriver: payload must be an object");
  const jsi::Object object = value.getObject(rt);
  const int32_t kind = intField(rt, object, "kind");
  storage.payload.kind = static_cast<pd_payload_kind>(kind);

  if (kind == PD_PAYLOAD_RASTER_RGBA8) {
    const jsi::Value pixelsValue = object.getProperty(rt, "pixels");
    if (!pixelsValue.isObject()) {
      throw jsi::JSError(rt, "printerdriver: raster payload has no pixel ArrayBuffer");
    }
    const jsi::Object holder = pixelsValue.getObject(rt);
    if (!holder.isArrayBuffer(rt)) {
      throw jsi::JSError(rt, "printerdriver: raster pixels must be an ArrayBuffer");
    }
    const jsi::ArrayBuffer buffer = holder.getArrayBuffer(rt);
    storage.pixels = buffer.data(rt);
    storage.payload.as.raster.pixels = storage.pixels;
    storage.payload.as.raster.width = uintField(rt, object, "width");
    storage.payload.as.raster.height = uintField(rt, object, "height");
    storage.payload.as.raster.stride_bytes = uintField(rt, object, "strideBytes");
    storage.payload.as.raster.binarization =
        static_cast<pd_binarization>(intField(rt, object, "binarization"));
    storage.payload.as.raster.threshold =
        static_cast<uint8_t>(intField(rt, object, "threshold"));
    storage.payload.as.raster.max_rows_per_band = uintField(rt, object, "maxRowsPerBand");
    return;
  }

  if (kind == PD_PAYLOAD_DOCUMENT) {
    const jsi::Value opsValue = object.getProperty(rt, "ops");
    if (!opsValue.isObject()) throw jsi::JSError(rt, "printerdriver: document has no ops");
    const jsi::Object holder = opsValue.getObject(rt);
    if (!holder.isArray(rt)) throw jsi::JSError(rt, "printerdriver: document ops must be an array");
    const jsi::Array array = holder.getArray(rt);
    const size_t count = array.size(rt);
    storage.texts.resize(count);
    storage.ops.resize(count);
    for (size_t index = 0; index < count; ++index) {
      const jsi::Value entry = array.getValueAtIndex(rt, index);
      const jsi::Object op = entry.getObject(rt);
      storage.ops[index].kind = static_cast<pd_op_kind>(intField(rt, op, "kind"));
      storage.ops[index].value = intField(rt, op, "value");
      const jsi::Value textValue = op.getProperty(rt, "text");
      if (textValue.isString()) {
        storage.texts[index] = textValue.getString(rt).utf8(rt);
        storage.ops[index].text = storage.texts[index].c_str();
      } else {
        // A null text on a `line` op is a bare LF, which is not the same as an empty
        // string, so the distinction is preserved rather than normalised away.
        storage.ops[index].text = nullptr;
      }
    }
    storage.payload.as.document.ops = storage.ops.data();
    storage.payload.as.document.count = storage.ops.size();
    storage.payload.as.document.code_page =
        static_cast<pd_code_page>(intField(rt, object, "codePage"));
    return;
  }

  storage.raw = bytesField(rt, object, "bytes");
  storage.payload.as.raw.bytes = storage.raw.data();
  storage.payload.as.raw.size = storage.raw.size();
}

// =========================================================================================
// the blocking-call worker
// =========================================================================================

using JsProducer = std::function<jsi::Value(jsi::Runtime&)>;

/**
 * Run `work` off the JS thread and resolve a JS Promise with whatever it produces.
 *
 * One detached thread per call rather than a pool, deliberately: every function routed
 * through here can block for as long as a printer takes to answer (pd_job_await with no
 * timeout waits indefinitely by design), so a bounded pool would let one parked job starve
 * every other printer. These calls are rare -- a print submission does not go through here,
 * only waits do.
 */
jsi::Value runAsync(const std::shared_ptr<ModuleState>& state, jsi::Runtime& rt,
                    std::function<JsProducer()> work) {
  jsi::Function promiseConstructor = rt.global().getPropertyAsFunction(rt, "Promise");
  jsi::Function executor = jsi::Function::createFromHostFunction(
      rt, jsi::PropNameID::forAscii(rt, "executor"), 2,
      [state, work](jsi::Runtime& runtime, const jsi::Value&, const jsi::Value* args,
                    size_t count) -> jsi::Value {
        if (count < 1 || !args[0].isObject()) return jsi::Value::undefined();
        auto resolve = std::make_shared<jsi::Function>(args[0].getObject(runtime).getFunction(runtime));
        std::weak_ptr<ModuleState> weak = state;
        std::thread([weak, work, resolve]() {
          JsProducer produced = work();
          const std::shared_ptr<ModuleState> live = weak.lock();
          if (!live || !live->jsInvoker) return;
          live->jsInvoker->invokeAsync([weak, produced, resolve]() {
            const std::shared_ptr<ModuleState> alive = weak.lock();
            if (!alive || alive->runtime == nullptr) return;
            jsi::Runtime& js = *alive->runtime;
            resolve->call(js, produced(js));
          });
        }).detach();
        return jsi::Value::undefined();
      });
  return promiseConstructor.callAsConstructor(rt, std::move(executor));
}

// =========================================================================================
// the method table
// =========================================================================================

using MethodBody =
    std::function<jsi::Value(ModuleState&, jsi::Runtime&, const jsi::Value*, size_t)>;

struct MethodEntry {
  const char* name;
  unsigned int paramCount;
  MethodBody body;
};

const std::vector<MethodEntry>& methodTable() {
  static const std::vector<MethodEntry>* table = new std::vector<MethodEntry>{
      // --- module plumbing --------------------------------------------------------------
      {"setEventSink", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         state.runtime = &rt;
         state.sink = std::make_shared<jsi::Function>(args[0].getObject(rt).getFunction(rt));
         return jsi::Value::undefined();
       }},
      {"respond", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const int32_t requestId = asInt(args[0]);
         std::shared_ptr<PendingRequest> pending;
         {
           std::lock_guard<std::mutex> guard(state.requests);
           const auto found = state.pending.find(requestId);
           if (found != state.pending.end()) pending = found->second;
         }
         if (!pending) return jsi::Value::undefined();
         if (args[1].isObject()) {
           const jsi::Object value = args[1].getObject(rt);
           std::lock_guard<std::mutex> guard(pending->mutex);
           if (value.hasProperty(rt, "ok")) pending->ok = boolField(rt, value, "ok");
           if (value.hasProperty(rt, "answered")) pending->ok = boolField(rt, value, "answered");
           if (value.hasProperty(rt, "handled")) pending->ok = boolField(rt, value, "handled");
           if (value.hasProperty(rt, "written")) {
             pending->number = value.getProperty(rt, "written").asNumber();
           }
           if (value.hasProperty(rt, "kind")) {
             pending->number = value.getProperty(rt, "kind").asNumber();
           }
           if (value.hasProperty(rt, "pinHigh")) {
             pending->number = value.getProperty(rt, "pinHigh").asNumber();
           }
           if (value.hasProperty(rt, "token")) pending->text = stringField(rt, value, "token");
           if (value.hasProperty(rt, "label")) pending->text = stringField(rt, value, "label");
           if (value.hasProperty(rt, "text")) pending->text = stringField(rt, value, "text");
           if (value.hasProperty(rt, "detail")) pending->text = stringField(rt, value, "detail");
           if (value.hasProperty(rt, "bytes")) pending->bytes = bytesField(rt, value, "bytes");
           pending->answered = true;
         } else {
           std::lock_guard<std::mutex> guard(pending->mutex);
           pending->answered = true;
         }
         pending->cv.notify_all();
         return jsi::Value::undefined();
       }},

      // --- driver -----------------------------------------------------------------------
      {"create", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const jsi::Object config = args[0].getObject(rt);
         const std::string storage = stringField(rt, config, "storageDirectory");
         pd_config native{};
         native.storage_directory = storage.empty() ? nullptr : storage.c_str();
         native.fsync_disabled = intField(rt, config, "fsyncDisabled");
         if (boolField(rt, config, "hasLog")) {
           native.log = &onLog;
           native.log_ctx = &state;
         }
         return makeNumber(registerDriver(state, pd_create(&native)));
       }},
      {"destroy", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         return runAsync(state.shared_from_this(), rt, [driver]() -> JsProducer {
           pd_destroy(driver);
           return [](jsi::Runtime&) { return jsi::Value::undefined(); };
         });
       }},
      {"lastError", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = driverAt(state, asInt(args[0]));
         return makeString(rt, driver == nullptr ? "" : pd_last_error(driver));
       }},
      {"profileIds", 0,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value*, size_t) -> jsi::Value {
         const char* const* ids = pd_profile_ids();
         size_t count = 0;
         while (ids != nullptr && ids[count] != nullptr) count += 1;
         jsi::Array array(rt, count);
         for (size_t index = 0; index < count; ++index) {
           array.setValueAtIndex(rt, index, makeString(rt, ids[index]));
         }
         return jsi::Value(std::move(array));
       }},
      {"instanceNonce", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_instance_nonce(requireDriver(state, rt, args[0])));
       }},
      {"localSubnet", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_local_subnet(requireDriver(state, rt, args[0])));
       }},

      // --- printers ---------------------------------------------------------------------
      {"addPrinterTcp", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object config = args[1].getObject(rt);
         const std::string printerId = stringField(rt, config, "printerId");
         const std::string host = stringField(rt, config, "host");
         const std::string profileId = stringField(rt, config, "profileId");
         pd_tcp_config native{};
         native.printer_id = printerId.empty() ? nullptr : printerId.c_str();
         native.host = host.c_str();
         native.port = static_cast<uint16_t>(uintField(rt, config, "port"));
         native.width_dots = uintField(rt, config, "widthDots");
         native.profile_id = profileId.empty() ? nullptr : profileId.c_str();
         native.connect_timeout_ms = uintField(rt, config, "connectTimeoutMs");
         return makeNumber(tokenForPrinter(state, pd_add_printer_tcp(driver, &native)));
       }},
      {"addPrinterCustom", 5,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         auto transport = std::make_unique<JsTransport>();
         transport->state = state.shared_from_this();
         transport->id = asInt(args[1]);
         auto description = std::make_unique<std::string>(asString(rt, args[2]));
         const std::string profileId = asString(rt, args[3]);
         pd_transport_vtable vtable{};
         vtable.connect = &transportConnect;
         vtable.write = &transportWrite;
         vtable.close = &transportClose;
         vtable.description = description->empty() ? nullptr : description->c_str();
         JsTransport* ctx = transport.get();
         // pd.h: the vtable is copied, but the function pointers and `ctx` must stay valid
         // until pd_destroy, so both live in the module for the driver's whole life.
         state.transports.push_back(std::move(transport));
         state.descriptions.push_back(std::move(description));
         pd_printer* printer = pd_add_printer_custom(
             driver, &vtable, ctx, profileId.empty() ? nullptr : profileId.c_str(),
             asUint(args[4]));
         return makeNumber(tokenForPrinter(state, printer));
       }},
      {"transportFeedBytes", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_printer* printer = requirePrinter(state, rt, args[0]);
         const jsi::Object holder = args[1].getObject(rt);
         if (!holder.isArrayBuffer(rt)) {
           throw jsi::JSError(rt, "printerdriver: feedBytes needs an ArrayBuffer");
         }
         const jsi::ArrayBuffer buffer = holder.getArrayBuffer(rt);
         return makeBool(pd_transport_feed_bytes(printer, buffer.data(rt), buffer.size(rt)) != 0);
       }},
      {"transportLinkDropped", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_printer* printer = requirePrinter(state, rt, args[0]);
         const std::string message = asString(rt, args[1]);
         return makeBool(pd_transport_link_dropped(printer, message.c_str()) != 0);
       }},
      {"printerId", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_printer_id(requirePrinter(state, rt, args[0])));
       }},
      {"printerWidthDots", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(pd_printer_width_dots(requirePrinter(state, rt, args[0])));
       }},
      {"printerCompletion", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<int32_t>(pd_printer_completion(requirePrinter(state, rt, args[0]))));
       }},
      {"printerCompletionProvenance", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(static_cast<int32_t>(
             pd_printer_completion_provenance(requirePrinter(state, rt, args[0]))));
       }},
      {"printerLanguage", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<int32_t>(pd_printer_language(requirePrinter(state, rt, args[0]))));
       }},
      {"printerStatus", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         return statusToJs(rt, pd_printer_status(driver, printer));
       }},
      {"printerRefreshStatus", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         const uint32_t timeout = asUint(args[2]);
         return runAsync(state.shared_from_this(), rt,
                         [driver, printer, timeout]() -> JsProducer {
                           const pd_device_status status =
                               pd_printer_refresh_status(driver, printer, timeout);
                           return [status](jsi::Runtime& js) { return statusToJs(js, status); };
                         });
       }},
      {"openCashDrawer", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_open_cash_drawer(requireDriver(state, rt, args[0]),
                             requirePrinter(state, rt, args[1]));
         return jsi::Value::undefined();
       }},
      {"printerDrain", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         return runAsync(state.shared_from_this(), rt, [driver, printer]() -> JsProducer {
           pd_printer_drain(driver, printer);
           return [](jsi::Runtime&) { return jsi::Value::undefined(); };
         });
       }},
      {"subscribeDevice", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_subscribe_device(requireDriver(state, rt, args[0]),
                             requirePrinter(state, rt, args[1]), &onDeviceEvent, &state);
         return jsi::Value::undefined();
       }},

      // --- jobs -------------------------------------------------------------------------
      {"print", 4,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         PayloadStorage storage;
         readPayload(rt, args[2], storage);
         const jsi::Object optionsObject = args[3].getObject(rt);
         const std::string key = stringField(rt, optionsObject, "key");
         pd_job_options options = readJobOptions(rt, args[3]);
         options.key = key.empty() ? nullptr : key.c_str();
         return makeNumber(
             tokenForJob(state, pd_print(driver, printer, &storage.payload, &options)));
       }},
      {"forceReprint", 4,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         const std::string key = asString(rt, args[2]);
         const jsi::Object optionsObject = args[3].getObject(rt);
         const std::string optionKey = stringField(rt, optionsObject, "key");
         pd_job_options options = readJobOptions(rt, args[3]);
         options.key = optionKey.empty() ? nullptr : optionKey.c_str();
         return makeNumber(
             tokenForJob(state, pd_force_reprint(driver, printer, key.c_str(), &options)));
       }},
      {"forceReprintOpts", 4,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         const std::string key = asString(rt, args[2]);
         const jsi::Object wrapper = args[3].getObject(rt);
         const jsi::Value jobValue = wrapper.getProperty(rt, "job");
         const jsi::Object jobObject = jobValue.getObject(rt);
         const std::string optionKey = stringField(rt, jobObject, "key");
         pd_reprint_options options{};
         options.job = readJobOptions(rt, jobValue);
         options.job.key = optionKey.empty() ? nullptr : optionKey.c_str();
         options.suppress_banner = intField(rt, wrapper, "suppressBanner");
         return makeNumber(
             tokenForJob(state, pd_force_reprint_opts(driver, printer, key.c_str(), &options)));
       }},
      {"findJob", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const std::string key = asString(rt, args[1]);
         return makeNumber(tokenForJob(state, pd_find_job(driver, key.c_str())));
       }},
      {"jobByToken", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const std::string token = asString(rt, args[1]);
         return makeNumber(tokenForJob(state, pd_job_by_token(driver, token.c_str())));
       }},
      {"jobId", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_job_id(requireJob(state, rt, args[0])));
       }},
      {"jobKey", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_job_key(requireJob(state, rt, args[0])));
       }},
      {"jobPrintToken", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_job_print_token(requireJob(state, rt, args[0])));
       }},
      {"jobCutToken", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_job_cut_token(requireJob(state, rt, args[0])));
       }},
      {"jobAttempt", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(pd_job_attempt(requireJob(state, rt, args[0])));
       }},
      {"jobCurrentState", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<int32_t>(pd_job_current_state(requireJob(state, rt, args[0]))));
       }},
      {"jobConfidence", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<int32_t>(pd_job_confidence(requireJob(state, rt, args[0]))));
       }},
      {"jobIsTerminal", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeBool(pd_job_is_terminal(requireJob(state, rt, args[0])) != 0);
       }},
      {"subscribeJob", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_subscribe_job(requireDriver(state, rt, args[0]), requireJob(state, rt, args[1]),
                          &onJobEvent, &state);
         return jsi::Value::undefined();
       }},
      {"jobAwait", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_job* job = requireJob(state, rt, args[1]);
         const uint32_t timeout = asUint(args[2]);
         return runAsync(state.shared_from_this(), rt, [driver, job, timeout]() -> JsProducer {
           pd_job_result result{};
           const int32_t ok = pd_job_await(driver, job, timeout, &result);
           if (ok == 0) return [](jsi::Runtime&) { return jsi::Value::null(); };
           // pd.h owns `method` until pd_destroy, but the JS thread runs later, so copy it
           // here rather than reading the pointer from another thread's future.
           const std::string method = safe(result.method);
           return [result, method](jsi::Runtime& js) { return resultToJs(js, result, method); };
         });
       }},

      // --- enum names -------------------------------------------------------------------
      {"jobStateName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_job_state_name(static_cast<pd_job_state>(asInt(args[0]))));
       }},
      {"confidenceLevelName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_confidence_level_name(static_cast<pd_confidence_level>(asInt(args[0]))));
       }},
      {"deviceEventName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt,
                           pd_device_event_name(static_cast<pd_device_event>(asInt(args[0]))));
       }},
      {"failureReasonName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_failure_reason_name(static_cast<pd_failure_reason>(asInt(args[0]))));
       }},
      {"jobOutcomeName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_job_outcome_name(static_cast<pd_job_outcome>(asInt(args[0]))));
       }},
      {"confidenceGradeName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_confidence_grade_name(static_cast<pd_confidence_grade>(asInt(args[0]))));
       }},
      {"confidenceGradeLetter", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_confidence_grade_letter(static_cast<pd_confidence_grade>(asInt(args[0]))));
       }},
      {"completionAuthorityName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_completion_authority_name(
                                   static_cast<pd_completion_authority>(asInt(args[0]))));
       }},
      {"provenanceName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_provenance_name(static_cast<pd_provenance>(asInt(args[0]))));
       }},
      {"commandLanguageName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_command_language_name(static_cast<pd_command_language>(asInt(args[0]))));
       }},
      {"payloadKindName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt,
                           pd_payload_kind_name(static_cast<pd_payload_kind>(asInt(args[0]))));
       }},
      {"completionMechanismName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_completion_mechanism_name(
                                   static_cast<pd_completion_mechanism>(asInt(args[0]))));
       }},
      {"cutVariantName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_cut_variant_name(static_cast<pd_cut_variant>(asInt(args[0]))));
       }},
      {"drainOrderName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_drain_order_name(static_cast<pd_drain_order>(asInt(args[0]))));
       }},
      {"drawerStateName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt,
                           pd_drawer_state_name(static_cast<pd_drawer_state>(asInt(args[0]))));
       }},
      {"drawerPortStandardName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_drawer_port_standard_name(
                                   static_cast<pd_drawer_port_standard>(asInt(args[0]))));
       }},
      {"drawerKickMethodName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_drawer_kick_method_name(
                                   static_cast<pd_drawer_kick_method>(asInt(args[0]))));
       }},
      {"drawerStatusMethodName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_drawer_status_method_name(
                                   static_cast<pd_drawer_status_method>(asInt(args[0]))));
       }},
      {"profileSelectionName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_profile_selection_name(static_cast<pd_profile_selection>(asInt(args[0]))));
       }},
      {"detectionStatusName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(
             rt, pd_detection_status_name(static_cast<pd_detection_status>(asInt(args[0]))));
       }},
      {"matchKindName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_match_kind_name(static_cast<pd_match_kind>(asInt(args[0]))));
       }},
      {"codePageAt", 1,
       [](ModuleState&, jsi::Runtime&, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(static_cast<int32_t>(pd_code_page_at(asInt(args[0]))));
       }},

      // --- print queue ------------------------------------------------------------------
      {"queueCreate", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         pd_queue_policy policy{};
         policy.hold_while_offline = intField(rt, object, "holdWhileOffline");
         policy.default_ttl_ms = uintField(rt, object, "defaultTtlMs");
         policy.max_depth = uintField(rt, object, "maxDepth");
         policy.drain_order = static_cast<pd_drain_order>(intField(rt, object, "drainOrder"));
         return makeNumber(registerQueue(state, pd_queue_create(driver, &policy)));
       }},
      {"queueDestroy", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_queue* queue = requireQueue(state, rt, args[0]);
         return runAsync(state.shared_from_this(), rt, [queue]() -> JsProducer {
           pd_queue_destroy(queue);
           return [](jsi::Runtime&) { return jsi::Value::undefined(); };
         });
       }},
      {"queueEnqueue", 4,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_queue* queue = requireQueue(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         PayloadStorage storage;
         readPayload(rt, args[2], storage);
         const jsi::Object optionsObject = args[3].getObject(rt);
         const std::string key = stringField(rt, optionsObject, "key");
         pd_queue_options options = readQueueOptions(rt, args[3]);
         options.key = key.empty() ? nullptr : key.c_str();
         return makeNumber(
             tokenForJob(state, pd_queue_enqueue(queue, printer, &storage.payload, &options)));
       }},
      {"queuePause", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const std::string printerId = asString(rt, args[1]);
         pd_queue_pause(requireQueue(state, rt, args[0]), printerId.c_str());
         return jsi::Value::undefined();
       }},
      {"queueResume", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const std::string printerId = asString(rt, args[1]);
         pd_queue_resume(requireQueue(state, rt, args[0]), printerId.c_str());
         return jsi::Value::undefined();
       }},
      {"queueIsPaused", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const std::string printerId = asString(rt, args[1]);
         return makeBool(pd_queue_is_paused(requireQueue(state, rt, args[0]),
                                            printerId.c_str()) != 0);
       }},
      {"queueIsBlocked", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const std::string printerId = asString(rt, args[1]);
         return makeBool(pd_queue_is_blocked(requireQueue(state, rt, args[0]),
                                             printerId.c_str()) != 0);
       }},
      {"queueUnblock", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const std::string printerId = asString(rt, args[1]);
         pd_queue_unblock(requireQueue(state, rt, args[0]), printerId.c_str());
         return jsi::Value::undefined();
       }},
      {"queuePending", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         const std::string printerId = asString(rt, args[1]);
         return makeNumber(static_cast<double>(
             pd_queue_pending(requireQueue(state, rt, args[0]),
                              printerId.empty() ? nullptr : printerId.c_str())));
       }},
      {"queueExpiredCount", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<double>(pd_queue_expired_count(requireQueue(state, rt, args[0]))));
       }},
      {"queueOverflowCount", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<double>(pd_queue_overflow_count(requireQueue(state, rt, args[0]))));
       }},
      {"queueDrainedCount", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<double>(pd_queue_drained_count(requireQueue(state, rt, args[0]))));
       }},
      {"queueTick", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_queue_tick(requireQueue(state, rt, args[0]));
         return jsi::Value::undefined();
       }},

      // --- cash drawer ------------------------------------------------------------------
      {"printerDrawerCapabilities", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return drawerCapabilitiesToJs(
             rt, pd_printer_drawer_capabilities(requirePrinter(state, rt, args[0])));
       }},
      {"drawerOpen", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         const jsi::Object object = args[2].getObject(rt);
         pd_drawer_request request{};
         request.channel = static_cast<uint8_t>(intField(rt, object, "channel"));
         request.pulse_ms = static_cast<uint16_t>(intField(rt, object, "pulseMs"));
         return runAsync(state.shared_from_this(), rt,
                         [driver, printer, request]() -> JsProducer {
                           const pd_drawer_result result =
                               pd_drawer_open(driver, printer, &request);
                           return [result](jsi::Runtime& js) {
                             return drawerResultToJs(js, result);
                           };
                         });
       }},
      {"drawerReadSensor", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         const uint32_t timeout = asUint(args[2]);
         return runAsync(state.shared_from_this(), rt,
                         [driver, printer, timeout]() -> JsProducer {
                           const pd_drawer_reading reading =
                               pd_drawer_read_sensor(driver, printer, timeout);
                           return [reading](jsi::Runtime& js) {
                             return drawerReadingToJs(js, reading);
                           };
                         });
       }},
      {"drawerCalibratePolarity", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeBool(pd_drawer_calibrate_polarity(requireDriver(state, rt, args[0]),
                                                      requirePrinter(state, rt, args[1]),
                                                      asBool(args[2]) ? 1 : 0) != 0);
       }},
      {"drawerPolarityCalibrated", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeBool(pd_drawer_polarity_calibrated(requireDriver(state, rt, args[0]),
                                                       requirePrinter(state, rt, args[1])) != 0);
       }},
      {"drawerHighMeansOpen", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeBool(pd_drawer_high_means_open(requireDriver(state, rt, args[0]),
                                                   requirePrinter(state, rt, args[1])) != 0);
       }},

      // --- self-test, auto-detection, discovery -----------------------------------------
      {"selfTest", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         const jsi::Object object = args[2].getObject(rt);
         auto key = std::make_shared<std::string>(stringField(rt, object, "key"));
         auto barcode = std::make_shared<std::string>(stringField(rt, object, "barcodeData"));
         pd_self_test_options options{};
         options.refresh_identity = intField(rt, object, "refreshIdentity");
         options.probe_without_printing = intField(rt, object, "probeWithoutPrinting");
         options.no_barcode = intField(rt, object, "noBarcode");
         options.no_verification_id = intField(rt, object, "noVerificationId");
         options.timeout_ms = uintField(rt, object, "timeoutMs");
         std::shared_ptr<ModuleState> shared = state.shared_from_this();
         return runAsync(shared, rt, [shared, driver, printer, options, key,
                                      barcode]() mutable -> JsProducer {
           options.key = key->empty() ? nullptr : key->c_str();
           options.barcode_data = barcode->empty() ? nullptr : barcode->c_str();
           pd_self_test_result out{};
           const int32_t ok = pd_self_test(driver, printer, &options, &out);
           if (ok == 0) return [](jsi::Runtime&) { return jsi::Value::null(); };
           // Every string in pd_self_test_result is owned by the driver and valid only
           // until the next pd_self_test on it, so all of them are copied here.
           const pd_job_result result = out.result;
           const std::string method = safe(out.result.method);
           const DetectionSummaryData detection = copySummary(out.detection);
           const std::string resultKey = safe(out.key);
           const std::string printToken = safe(out.print_token);
           const std::string ticket = safe(out.ticket_text);
           const int32_t jobToken = tokenForJob(*shared, out.job);
           return [result, method, detection, resultKey, printToken, ticket,
                   jobToken](jsi::Runtime& js) {
             jsi::Object object(js);
             object.setProperty(js, "result", resultToJs(js, result, method));
             object.setProperty(js, "detection", summaryToJs(js, detection));
             object.setProperty(js, "key", makeString(js, resultKey));
             object.setProperty(js, "printToken", makeString(js, printToken));
             object.setProperty(js, "ticketText", makeString(js, ticket));
             object.setProperty(js, "job", makeNumber(jobToken));
             return jsi::Value(std::move(object));
           };
         });
       }},
      {"autoDetect", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto cidr = std::make_shared<std::string>(stringField(rt, object, "subnetCidr"));
         auto endpoints =
             std::make_shared<std::vector<std::string>>(stringArrayField(rt, object, "endpoints"));
         pd_auto_detect_options options{};
         options.port = static_cast<uint16_t>(uintField(rt, object, "port"));
         options.concurrency = uintField(rt, object, "concurrency");
         options.connect_timeout_ms = uintField(rt, object, "connectTimeoutMs");
         options.response_timeout_ms = uintField(rt, object, "responseTimeoutMs");
         options.leave_unknown_unprobed = intField(rt, object, "leaveUnknownUnprobed");
         options.status_timeout_ms = uintField(rt, object, "statusTimeoutMs");
         options.identity_timeout_ms = uintField(rt, object, "identityTimeoutMs");
         options.completion_timeout_ms = uintField(rt, object, "completionTimeoutMs");
         auto sweep = std::make_shared<SweepContext>();
         sweep->state = state.shared_from_this();
         sweep->requestId = asInt(args[2]);
         return runAsync(state.shared_from_this(), rt,
                         [driver, options, cidr, endpoints, sweep]() mutable -> JsProducer {
                           std::vector<const char*> pointers;
                           for (const std::string& endpoint : *endpoints) {
                             pointers.push_back(endpoint.c_str());
                           }
                           pointers.push_back(nullptr);
                           options.subnet_cidr = cidr->empty() ? nullptr : cidr->c_str();
                           options.endpoints = endpoints->empty() ? nullptr : pointers.data();
                           const int32_t count =
                               pd_auto_detect(driver, &options, &onDetected, sweep.get());
                           return [count](jsi::Runtime&) { return makeNumber(count); };
                         });
       }},
      {"discover", 3,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto cidr = std::make_shared<std::string>(stringField(rt, object, "subnetCidr"));
         pd_discover_options options{};
         options.port = static_cast<uint16_t>(uintField(rt, object, "port"));
         options.concurrency = uintField(rt, object, "concurrency");
         options.connect_timeout_ms = uintField(rt, object, "connectTimeoutMs");
         options.response_timeout_ms = uintField(rt, object, "responseTimeoutMs");
         options.no_backchannel_probe = intField(rt, object, "noBackchannelProbe");
         auto sweep = std::make_shared<SweepContext>();
         sweep->state = state.shared_from_this();
         sweep->requestId = asInt(args[2]);
         return runAsync(state.shared_from_this(), rt,
                         [driver, options, cidr, sweep]() mutable -> JsProducer {
                           options.subnet_cidr = cidr->empty() ? nullptr : cidr->c_str();
                           const int32_t count =
                               pd_discover(driver, &options, &onDiscovered, sweep.get());
                           return [count](jsi::Runtime&) { return makeNumber(count); };
                         });
       }},
      {"detectedAt", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_detected_printer out{};
         if (pd_detected_at(driver, asInt(args[1]), &out) == 0) return jsi::Value::null();
         return detectedToJs(rt, copyDetected(out));
       }},
      {"discoveredAt", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_discovered_device out{};
         if (pd_discovered_at(driver, asInt(args[1]), &out) == 0) return jsi::Value::null();
         return discoveredToJs(rt, copyDiscovered(out));
       }},

      // --- custom method registration ---------------------------------------------------
      {"registerCompletionMethod", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto registration = std::make_unique<JsRegistration>();
         registration->state = state.shared_from_this();
         registration->id = stringField(rt, object, "id");
         const std::string methodName = stringField(rt, object, "methodName");
         pd_completion_method method{};
         method.id = registration->id.c_str();
         method.fence_bytes = &fenceBytesThunk;
         method.matcher = &matcherThunk;
         method.ctx = registration.get();
         method.grade = static_cast<pd_confidence_grade>(intField(rt, object, "grade"));
         method.authority =
             static_cast<pd_completion_authority>(intField(rt, object, "authority"));
         method.method_name = methodName.empty() ? nullptr : methodName.c_str();
         const int32_t ok = pd_register_completion_method(driver, &method);
         if (ok != 0) state.registrations.push_back(std::move(registration));
         return makeBool(ok != 0);
       }},
      {"registerProbeStep", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto registration = std::make_unique<JsRegistration>();
         registration->state = state.shared_from_this();
         registration->id = stringField(rt, object, "id");
         auto bytes = std::make_unique<std::vector<uint8_t>>(bytesField(rt, object, "requestBytes"));
         pd_probe_step step{};
         step.id = registration->id.c_str();
         step.request_bytes = bytes->data();
         step.request_size = bytes->size();
         step.classify = &classifyThunk;
         step.ctx = registration.get();
         const int32_t ok = pd_register_probe_step(driver, &step);
         if (ok != 0) {
           state.registrations.push_back(std::move(registration));
           state.probeRequests.push_back(std::move(bytes));
         }
         return makeBool(ok != 0);
       }},
      {"registerBlockHandler", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto registration = std::make_unique<JsRegistration>();
         registration->state = state.shared_from_this();
         registration->id = stringField(rt, object, "kind");
         pd_block_handler handler{};
         handler.kind = registration->id.c_str();
         handler.handler = &blockHandlerThunk;
         handler.ctx = registration.get();
         const int32_t ok = pd_register_block_handler(driver, &handler);
         if (ok != 0) state.registrations.push_back(std::move(registration));
         return makeBool(ok != 0);
       }},
      {"registerFormatter", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto registration = std::make_unique<JsRegistration>();
         registration->state = state.shared_from_this();
         registration->id = stringField(rt, object, "name");
         pd_formatter formatter{};
         formatter.name = registration->id.c_str();
         formatter.formatter = &formatterThunk;
         formatter.ctx = registration.get();
         const int32_t ok = pd_register_formatter(driver, &formatter);
         if (ok != 0) state.registrations.push_back(std::move(registration));
         return makeBool(ok != 0);
       }},
      {"registerDrawerKick", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         const jsi::Object object = args[1].getObject(rt);
         auto registration = std::make_unique<JsRegistration>();
         registration->state = state.shared_from_this();
         registration->id = stringField(rt, object, "id");
         registration->hasStatus = boolField(rt, object, "hasStatus");
         pd_drawer_kick_reg reg{};
         reg.id = registration->id.c_str();
         reg.kick_bytes = &drawerKickBytesThunk;
         // pd.h: both status callbacks are optional and GO TOGETHER. Both null means the
         // vendor method has no readable switch, so a kick reports kickSentUnverified
         // rather than a verified open -- which is the honest answer, not a downgrade.
         reg.status_request = registration->hasStatus ? &drawerStatusRequestThunk : nullptr;
         reg.status_parse = registration->hasStatus ? &drawerStatusParseThunk : nullptr;
         reg.ctx = registration.get();
         const int32_t ok = pd_register_drawer_kick(driver, &reg);
         if (ok != 0) state.registrations.push_back(std::move(registration));
         return makeBool(ok != 0);
       }},

      // --- the receipt DSL (M19) --------------------------------------------------------
      //
      // Both of these run on the worker, and the reason is stronger than "rendering takes
      // time": a document may reach a formatter or a block handler registered from
      // JavaScript, and those park a core thread on the sink until `respond` comes back. On
      // the JS thread that is a deadlock against itself. Every string handed to the ABI and
      // every string read back out is copied on the worker for the same reason -- the JS
      // thread runs later, and by then the driver may have rendered something else.
      {"renderDocument", 5,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         auto document = std::make_shared<std::string>(asString(rt, args[2]));
         auto model = std::make_shared<std::string>(asString(rt, args[3]));
         auto options = std::make_shared<RenderOptionsStorage>(readRenderOptions(rt, args[4]));
         return runAsync(state.shared_from_this(), rt,
                         [driver, printer, document, model, options]() -> JsProducer {
                           options->rebind();
                           pd_render_result out{};
                           const int32_t ok = pd_render_document(
                               driver, printer, document->c_str(),
                               model->empty() ? nullptr : model->c_str(), &options->options,
                               &out);
                           // The reason is read FIRST, while it is still the reason this
                           // call failed and not whatever the next ABI call left behind.
                           const std::string error =
                               ok == 1 ? std::string() : safe(pd_last_error(driver));
                           std::vector<uint8_t> bytes;
                           if (ok == 1 && out.bytes != nullptr && out.size > 0) {
                             bytes.assign(out.bytes, out.bytes + out.size);
                           }
                           // Read on BOTH paths: pd.h fills the report when it refuses, so
                           // a caller sees the same list it would see for a degradation.
                           const std::vector<ReportEntryData> report = readRenderReport(driver);
                           const int32_t codePage = static_cast<int32_t>(out.code_page);
                           const int32_t hasCut = out.has_cut;
                           const int32_t cut = static_cast<int32_t>(out.cut);
                           const double topFeed = static_cast<double>(out.top_feed_dots);
                           const double bottomFeed = static_cast<double>(out.bottom_feed_dots);
                           return [ok, bytes, report, error, codePage, hasCut, cut, topFeed,
                                   bottomFeed](jsi::Runtime& js) {
                             jsi::Object object(js);
                             object.setProperty(js, "ok", makeNumber(ok));
                             object.setProperty(js, "bytes",
                                                makeBytes(js, bytes.data(), bytes.size()));
                             object.setProperty(js, "codePage", makeNumber(codePage));
                             object.setProperty(js, "hasCut", makeNumber(hasCut));
                             object.setProperty(js, "cut", makeNumber(cut));
                             object.setProperty(js, "topFeedDots", makeNumber(topFeed));
                             object.setProperty(js, "bottomFeedDots", makeNumber(bottomFeed));
                             object.setProperty(js, "report", reportToJs(js, report));
                             object.setProperty(js, "error", makeString(js, error));
                             return jsi::Value(std::move(object));
                           };
                         });
       }},
      {"printDocumentJson", 5,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_printer* printer = requirePrinter(state, rt, args[1]);
         auto document = std::make_shared<std::string>(asString(rt, args[2]));
         auto model = std::make_shared<std::string>(asString(rt, args[3]));
         auto options = std::make_shared<JobOptionsStorage>(readJobOptionsStorage(rt, args[4]));
         std::shared_ptr<ModuleState> shared = state.shared_from_this();
         return runAsync(shared, rt,
                         [shared, driver, printer, document, model, options]() -> JsProducer {
                           options->rebind();
                           // out_result is NULL on purpose: everything it carries is either
                           // applied by the engine itself or readable from the report, and
                           // a struct of driver-owned pointers must not outlive this call.
                           pd_job* job = pd_print_document_json(
                               driver, printer, document->c_str(),
                               model->empty() ? nullptr : model->c_str(), &options->options,
                               nullptr);
                           const std::string error =
                               job == nullptr ? safe(pd_last_error(driver)) : std::string();
                           const std::vector<ReportEntryData> report = readRenderReport(driver);
                           const int32_t token = tokenForJob(*shared, job);
                           return [token, report, error](jsi::Runtime& js) {
                             jsi::Object object(js);
                             object.setProperty(js, "job", makeNumber(token));
                             object.setProperty(js, "report", reportToJs(js, report));
                             object.setProperty(js, "error", makeString(js, error));
                             return jsi::Value(std::move(object));
                           };
                         });
       }},
      {"renderReportAt", 2,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         pd_driver* driver = requireDriver(state, rt, args[0]);
         pd_report_entry out{};
         if (pd_render_report_at(driver, asInt(args[1]), &out) != 1) return jsi::Value::null();
         return reportEntryToJs(rt, copyReportEntry(out));
       }},
      {"renderReportCount", 1,
       [](ModuleState& state, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeNumber(
             static_cast<double>(pd_render_report_count(requireDriver(state, rt, args[0]))));
       }},
      {"reportKindName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_report_kind_name(static_cast<pd_report_kind>(asInt(args[0]))));
       }},
      {"renderPathName", 1,
       [](ModuleState&, jsi::Runtime& rt, const jsi::Value* args, size_t) -> jsi::Value {
         return makeString(rt, pd_render_path_name(static_cast<pd_render_path>(asInt(args[0]))));
       }},
  };
  return *table;
}

const MethodEntry* findMethod(const std::string& name) {
  for (const MethodEntry& entry : methodTable()) {
    if (name == entry.name) return &entry;
  }
  return nullptr;
}

}  // namespace

// =========================================================================================
// PrinterDriverModule
// =========================================================================================

PrinterDriverModule::PrinterDriverModule(std::shared_ptr<CallInvoker> jsInvoker)
    : facebook::react::TurboModule("PrinterDriver", jsInvoker),
      state_(std::make_shared<ModuleState>()) {
  state_->jsInvoker = std::move(jsInvoker);
}

PrinterDriverModule::~PrinterDriverModule() {
  // The runtime is going away; nothing may touch it again. Anything still blocked in `ask`
  // times out and takes its registration's documented failure, which is correct: a driver
  // whose JavaScript half has been torn down cannot answer.
  state_->runtime = nullptr;
  state_->sink.reset();
}

jsi::Value PrinterDriverModule::get(jsi::Runtime& runtime, const jsi::PropNameID& propName) {
  const std::string name = propName.utf8(runtime);
  const MethodEntry* entry = findMethod(name);
  if (entry == nullptr) return jsi::Value::undefined();
  state_->runtime = &runtime;
  std::shared_ptr<ModuleState> state = state_;
  const MethodBody body = entry->body;
  return jsi::Value(jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forUtf8(runtime, name), entry->paramCount,
      [state, body](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                    size_t count) -> jsi::Value { return body(*state, rt, args, count); }));
}

std::vector<jsi::PropNameID> PrinterDriverModule::getPropertyNames(jsi::Runtime& runtime) {
  std::vector<jsi::PropNameID> names;
  for (const MethodEntry& entry : methodTable()) {
    names.push_back(jsi::PropNameID::forAscii(runtime, entry.name));
  }
  return names;
}

std::shared_ptr<facebook::react::TurboModule> PrinterDriverModuleProvider(
    const std::string& name, const std::shared_ptr<CallInvoker>& jsInvoker) {
  if (name != "PrinterDriver") return nullptr;
  return std::make_shared<PrinterDriverModule>(jsInvoker);
}

}  // namespace pdrn
