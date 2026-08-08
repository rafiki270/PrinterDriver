#include "printerdriver/pd.h"

#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "pd_internal.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/transport.hpp"
#include "printerdriver/types.hpp"

// --- The mirroring guarantee -------------------------------------------------------
//
// docs/api.md §1.3: the enums are closed, defined once in the core, and re-exported
// verbatim by every wrapper. This translation unit is where that stops being a promise
// and becomes a build error. Every C constant is asserted against its C++ member, and
// every _COUNT against the core's kAll* array, so adding a value to the core without
// regenerating pd.h fails to compile here rather than reaching a wrapper as a silent
// hole.

namespace {

template <typename Enum>
constexpr int value_of(Enum member) {
  return static_cast<int>(member);
}

}  // namespace

static_assert(PD_JOB_STATE_QUEUED == value_of(pd::JobState::Queued));
static_assert(PD_JOB_STATE_PREFLIGHT_OK == value_of(pd::JobState::PreflightOk));
static_assert(PD_JOB_STATE_SEND_STARTED == value_of(pd::JobState::SendStarted));
static_assert(PD_JOB_STATE_BYTES_SENT == value_of(pd::JobState::BytesSent));
static_assert(PD_JOB_STATE_PRINT_CONFIRMED == value_of(pd::JobState::PrintConfirmed));
static_assert(PD_JOB_STATE_CUT_COMMAND_PROCESSED ==
              value_of(pd::JobState::CutCommandProcessed));
static_assert(PD_JOB_STATE_DONE_SOFTWARE == value_of(pd::JobState::DoneSoftware));
static_assert(PD_JOB_STATE_PHYSICALLY_VERIFIED ==
              value_of(pd::JobState::PhysicallyVerified));
static_assert(PD_JOB_STATE_FAILED_KNOWN == value_of(pd::JobState::FailedKnown));
static_assert(PD_JOB_STATE_UNKNOWN == value_of(pd::JobState::Unknown));
static_assert(PD_JOB_STATE_HELD_OFFLINE == value_of(pd::JobState::HeldOffline));
static_assert(PD_JOB_STATE_COUNT == static_cast<int>(pd::kAllJobStates.size()));

static_assert(PD_CONFIDENCE_TRANSPORT_ACCEPTED ==
              value_of(pd::ConfidenceLevel::TransportAccepted));
static_assert(PD_CONFIDENCE_PRINTER_HEALTHY ==
              value_of(pd::ConfidenceLevel::PrinterHealthy));
static_assert(PD_CONFIDENCE_PRINT_CONFIRMED ==
              value_of(pd::ConfidenceLevel::PrintConfirmed));
static_assert(PD_CONFIDENCE_CUT_PROCESSED == value_of(pd::ConfidenceLevel::CutProcessed));
static_assert(PD_CONFIDENCE_CUT_FAULT_FREE == value_of(pd::ConfidenceLevel::CutFaultFree));
static_assert(PD_CONFIDENCE_PHYSICALLY_VERIFIED ==
              value_of(pd::ConfidenceLevel::PhysicallyVerified));
static_assert(PD_CONFIDENCE_COUNT == static_cast<int>(pd::kAllConfidenceLevels.size()));

static_assert(PD_DEVICE_ONLINE == value_of(pd::DeviceEvent::Online));
static_assert(PD_DEVICE_OFFLINE == value_of(pd::DeviceEvent::Offline));
static_assert(PD_DEVICE_COVER_OPEN == value_of(pd::DeviceEvent::CoverOpen));
static_assert(PD_DEVICE_COVER_CLOSED == value_of(pd::DeviceEvent::CoverClosed));
static_assert(PD_DEVICE_PAPER_OUT == value_of(pd::DeviceEvent::PaperOut));
static_assert(PD_DEVICE_PAPER_NEAR_END == value_of(pd::DeviceEvent::PaperNearEnd));
static_assert(PD_DEVICE_PAPER_OK == value_of(pd::DeviceEvent::PaperOk));
static_assert(PD_DEVICE_CUTTER_ERROR == value_of(pd::DeviceEvent::CutterError));
static_assert(PD_DEVICE_RECOVERABLE_ERROR == value_of(pd::DeviceEvent::RecoverableError));
static_assert(PD_DEVICE_UNRECOVERABLE_ERROR ==
              value_of(pd::DeviceEvent::UnrecoverableError));
static_assert(PD_DEVICE_CONNECTION_LOST == value_of(pd::DeviceEvent::ConnectionLost));
static_assert(PD_DEVICE_CONNECTION_RESTORED ==
              value_of(pd::DeviceEvent::ConnectionRestored));
static_assert(PD_DEVICE_EVENT_COUNT == static_cast<int>(pd::kAllDeviceEvents.size()));

static_assert(PD_REASON_NONE == value_of(pd::FailureReason::None));
static_assert(PD_REASON_TRANSPORT_UNREACHABLE ==
              value_of(pd::FailureReason::TransportUnreachable));
static_assert(PD_REASON_PREFLIGHT_COVER_OPEN ==
              value_of(pd::FailureReason::PreflightCoverOpen));
static_assert(PD_REASON_PREFLIGHT_PAPER_OUT ==
              value_of(pd::FailureReason::PreflightPaperOut));
static_assert(PD_REASON_PREFLIGHT_HARDWARE_ERROR ==
              value_of(pd::FailureReason::PreflightHardwareError));
static_assert(PD_REASON_TIMEOUT_AWAITING_COMPLETION ==
              value_of(pd::FailureReason::TimeoutAwaitingCompletion));
static_assert(PD_REASON_CUTTER_FAULT == value_of(pd::FailureReason::CutterFault));
static_assert(PD_REASON_UNSUPPORTED == value_of(pd::FailureReason::Unsupported));
static_assert(PD_REASON_UNKNOWN == value_of(pd::FailureReason::Unknown));
static_assert(PD_REASON_EXPIRED == value_of(pd::FailureReason::Expired));
static_assert(PD_REASON_QUEUE_OVERFLOW == value_of(pd::FailureReason::QueueOverflow));
static_assert(PD_REASON_COUNT == static_cast<int>(pd::kAllFailureReasons.size()));

static_assert(PD_OUTCOME_DONE == value_of(pd::JobOutcome::Done));
static_assert(PD_OUTCOME_FAILED == value_of(pd::JobOutcome::Failed));
static_assert(PD_OUTCOME_UNKNOWN == value_of(pd::JobOutcome::Unknown));
static_assert(PD_OUTCOME_COUNT == static_cast<int>(pd::kAllJobOutcomes.size()));

static_assert(PD_CUT_PROFILE == value_of(pd::CutSetting::Profile));
static_assert(PD_CUT_PARTIAL == value_of(pd::CutSetting::Partial));
static_assert(PD_CUT_FULL == value_of(pd::CutSetting::Full));
static_assert(PD_CUT_NONE == value_of(pd::CutSetting::None));

static_assert(PD_PREFLIGHT_STRICT == value_of(pd::PreflightMode::Strict));
static_assert(PD_PREFLIGHT_SKIP == value_of(pd::PreflightMode::Skip));

static_assert(PD_PAYLOAD_RASTER_RGBA8 == value_of(pd::PayloadKind::Raster));
static_assert(PD_PAYLOAD_DOCUMENT == value_of(pd::PayloadKind::Document));
static_assert(PD_PAYLOAD_RAW == value_of(pd::PayloadKind::Raw));

static_assert(PD_COMPLETION_GS_PAREN_H == value_of(pd::CompletionMechanism::GsParenH));
static_assert(PD_COMPLETION_GS_R1 == value_of(pd::CompletionMechanism::GsR1));
static_assert(PD_COMPLETION_NONE == value_of(pd::CompletionMechanism::None));

static_assert(PD_CUT_VARIANT_PARTIAL == value_of(pd::CutVariant::Partial));
static_assert(PD_CUT_VARIANT_FULL == value_of(pd::CutVariant::Full));
static_assert(PD_CUT_VARIANT_NONE == value_of(pd::CutVariant::None));

static_assert(PD_ALIGN_LEFT == value_of(pd::escpos::Alignment::Left));
static_assert(PD_ALIGN_CENTER == value_of(pd::escpos::Alignment::Center));
static_assert(PD_ALIGN_RIGHT == value_of(pd::escpos::Alignment::Right));

static_assert(PD_CODE_PAGE_PC437 == value_of(pd::escpos::CodePage::PC437));
static_assert(PD_CODE_PAGE_PC850 == value_of(pd::escpos::CodePage::PC850));
static_assert(PD_CODE_PAGE_WPC1252 == value_of(pd::escpos::CodePage::WPC1252));
static_assert(PD_CODE_PAGE_PC852 == value_of(pd::escpos::CodePage::PC852));
static_assert(PD_CODE_PAGE_PC858 == value_of(pd::escpos::CodePage::PC858));

static_assert(PD_BINARIZATION_FIXED_THRESHOLD ==
              value_of(pd::escpos::Binarization::FixedThreshold));
static_assert(PD_BINARIZATION_FLOYD_STEINBERG ==
              value_of(pd::escpos::Binarization::FloydSteinberg));

namespace {

// --- Name tables -------------------------------------------------------------------
//
// Written out by hand rather than delegated to pd::to_string, so that the two
// spellings are genuinely independent and a reordering shows up as a mismatch instead
// of agreeing with itself. capi/tests/test_capi.c compares them member by member.

const char* const kJobStateNames[PD_JOB_STATE_COUNT] = {
    "Queued",      "PreflightOk",         "SendStarted", "BytesSent",
    "PrintConfirmed", "CutCommandProcessed", "DoneSoftware", "PhysicallyVerified",
    "FailedKnown", "Unknown",             "HeldOffline"};

const char* const kConfidenceNames[PD_CONFIDENCE_COUNT] = {
    "TransportAccepted", "PrinterHealthy", "PrintConfirmed",
    "CutProcessed",      "CutFaultFree",   "PhysicallyVerified"};

const char* const kDeviceEventNames[PD_DEVICE_EVENT_COUNT] = {
    "Online",       "Offline",           "CoverOpen",      "CoverClosed",
    "PaperOut",     "PaperNearEnd",      "PaperOk",        "CutterError",
    "RecoverableError", "UnrecoverableError", "ConnectionLost", "ConnectionRestored"};

const char* const kFailureReasonNames[PD_REASON_COUNT] = {
    "None",
    "TransportUnreachable",
    "PreflightCoverOpen",
    "PreflightPaperOut",
    "PreflightHardwareError",
    "TimeoutAwaitingCompletion",
    "CutterFault",
    "Unsupported",
    "Unknown",
    "Expired",
    "QueueOverflow"};

const char* const kJobOutcomeNames[PD_OUTCOME_COUNT] = {"Done", "Failed", "Unknown"};

const char* const kPayloadKindNames[PD_PAYLOAD_KIND_COUNT] = {"Raster", "Document", "Raw"};

const char* const kCompletionNames[PD_COMPLETION_COUNT] = {"GsParenH", "GsR1", "None"};

const char* const kCutVariantNames[PD_CUT_VARIANT_COUNT] = {"Partial", "Full", "None"};

const pd_code_page kCodePages[PD_CODE_PAGE_COUNT] = {
    PD_CODE_PAGE_PC437, PD_CODE_PAGE_PC850, PD_CODE_PAGE_WPC1252, PD_CODE_PAGE_PC852,
    PD_CODE_PAGE_PC858};

const char* const kProfileIds[] = {"generic", "xp-s260m", nullptr};

// --- Helpers -----------------------------------------------------------------------

void setError(pd_driver* driver, const std::string& message) {
  if (driver == nullptr) {
    return;
  }
  pd_log_cb log = nullptr;
  void* log_ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(driver->mutex);
    driver->last_error = message;
    log = driver->log;
    log_ctx = driver->log_ctx;
  }
  // Outside the lock: a log hook is caller code and may do anything, including calling
  // straight back in.
  if (log != nullptr && !message.empty()) {
    log(message.c_str(), log_ctx);
  }
}

void clearError(pd_driver* driver) {
  if (driver == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(driver->mutex);
  driver->last_error.clear();
}

bool checkHandles(pd_driver* driver, pd_printer* printer) {
  if (driver == nullptr) {
    return false;
  }
  if (printer == nullptr || printer->owner != driver || !printer->printer) {
    setError(driver, "printer handle does not belong to this driver");
    return false;
  }
  clearError(driver);
  return true;
}

bool checkJob(pd_driver* driver, pd_job* job) {
  if (driver == nullptr) {
    return false;
  }
  if (job == nullptr || job->owner != driver || !job->job) {
    setError(driver, "job handle does not belong to this driver");
    return false;
  }
  clearError(driver);
  return true;
}

int32_t tristate(const std::optional<bool>& value) {
  if (!value.has_value()) {
    return PD_UNKNOWN;
  }
  return *value ? PD_TRUE : PD_FALSE;
}

pd_device_status toStatus(const pd::DeviceStatus& status) {
  pd_device_status out;
  out.connected = status.connected ? PD_TRUE : PD_FALSE;
  out.observed = status.observed ? PD_TRUE : PD_FALSE;
  out.online = tristate(status.online);
  out.cover_open = tristate(status.cover_open);
  out.paper_out = tristate(status.paper_out);
  out.paper_near_end = tristate(status.paper_near_end);
  out.cutter_error = tristate(status.cutter_error);
  out.unrecoverable_error = tristate(status.unrecoverable_error);
  out.recoverable_error = tristate(status.recoverable_error);
  return out;
}

pd_device_status emptyStatus() {
  pd::DeviceStatus blank;
  return toStatus(blank);
}

pd_job_event toEvent(const pd::JobEvent& event) {
  pd_job_event out;
  out.state = static_cast<pd_job_state>(event.state);
  out.confidence = static_cast<pd_confidence_level>(event.confidence);
  out.has_reason = event.reason.has_value() ? 1 : 0;
  out.reason = static_cast<pd_failure_reason>(
      event.reason.has_value() ? static_cast<int>(*event.reason) : PD_REASON_NONE);
  out.monotonic_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(event.at.time_since_epoch())
          .count());
  return out;
}

pd::JobOptions toOptions(const pd_job_options* options) {
  pd::JobOptions out;
  if (options == nullptr) {
    return out;
  }
  if (options->key != nullptr) {
    out.key = options->key;  // copied here: the caller's buffer is not retained
  }
  out.cut = static_cast<pd::CutSetting>(options->cut);
  out.open_drawer = options->open_drawer != 0;
  out.preflight = static_cast<pd::PreflightMode>(options->preflight);
  out.timeout_ms = options->timeout_ms;
  return out;
}

// RGBA8 over white paper, then ITU-R 601 luma. Integer throughout, because two runs of
// the same receipt must produce byte-identical output for a golden test to mean
// anything.
std::vector<uint8_t> toGrayscale(const pd_raster_rgba8& raster) {
  const uint32_t stride =
      raster.stride_bytes != 0 ? raster.stride_bytes : raster.width * 4u;
  std::vector<uint8_t> gray(static_cast<size_t>(raster.width) * raster.height, 255);
  for (uint32_t y = 0; y < raster.height; ++y) {
    const uint8_t* row = raster.pixels + static_cast<size_t>(y) * stride;
    for (uint32_t x = 0; x < raster.width; ++x) {
      const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
      const uint32_t alpha = pixel[3];
      const uint32_t inverse = 255u - alpha;
      const uint32_t r = (pixel[0] * alpha + 255u * inverse + 127u) / 255u;
      const uint32_t g = (pixel[1] * alpha + 255u * inverse + 127u) / 255u;
      const uint32_t b = (pixel[2] * alpha + 255u * inverse + 127u) / 255u;
      gray[static_cast<size_t>(y) * raster.width + x] =
          static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
    }
  }
  return gray;
}

bool buildPayload(pd_driver* driver, const pd_payload* payload, pd::Payload* out) {
  if (payload == nullptr) {
    setError(driver, "payload is null");
    return false;
  }
  switch (payload->kind) {
    case PD_PAYLOAD_RASTER_RGBA8: {
      const pd_raster_rgba8& raster = payload->as.raster;
      if (raster.pixels == nullptr || raster.width == 0 || raster.height == 0) {
        setError(driver, "raster payload needs pixels and a non-zero size");
        return false;
      }
      pd::RasterPayload tier;
      tier.gray = toGrayscale(raster);
      tier.width = raster.width;
      tier.height = raster.height;
      tier.binarization = static_cast<pd::escpos::Binarization>(raster.binarization);
      tier.threshold = raster.threshold != 0 ? raster.threshold : 128;
      tier.max_rows_per_band =
          raster.max_rows_per_band != 0 ? raster.max_rows_per_band : 1024;
      *out = pd::Payload::raster(std::move(tier));
      return true;
    }
    case PD_PAYLOAD_DOCUMENT: {
      const pd_document& document = payload->as.document;
      if (document.count != 0 && document.ops == nullptr) {
        setError(driver, "document payload has a null op array");
        return false;
      }
      const auto code_page = static_cast<pd::escpos::CodePage>(document.code_page);
      pd::escpos::Encoder encoder;
      encoder.selectCodePage(code_page);
      for (size_t i = 0; i < document.count; ++i) {
        const pd_op& op = document.ops[i];
        switch (op.kind) {
          case PD_OP_TEXT:
            encoder.text(op.text != nullptr ? op.text : "");
            break;
          case PD_OP_LINE:
            encoder.line(op.text != nullptr ? op.text : "");
            break;
          case PD_OP_ALIGN:
            if (op.value < 0 || op.value >= PD_ALIGN_COUNT) {
              setError(driver, "document op " + std::to_string(i) +
                                   ": alignment out of range");
              return false;
            }
            encoder.align(static_cast<pd::escpos::Alignment>(op.value));
            break;
          case PD_OP_BOLD:
            encoder.bold(op.value != 0);
            break;
          case PD_OP_FEED:
            if (op.value < 1 || op.value > 255) {
              setError(driver,
                       "document op " + std::to_string(i) + ": feed lines out of range");
              return false;
            }
            encoder.feedLines(static_cast<uint8_t>(op.value));
            break;
          case PD_OP_KIND_COUNT:
          default:
            setError(driver, "document op " + std::to_string(i) + ": unknown kind");
            return false;
        }
      }
      *out = pd::Payload::document(encoder.take(), code_page);
      return true;
    }
    case PD_PAYLOAD_RAW: {
      const pd_raw& raw = payload->as.raw;
      if (raw.size != 0 && raw.bytes == nullptr) {
        setError(driver, "raw payload has a null byte array");
        return false;
      }
      *out = pd::Payload::raw(pd::escpos::Bytes(raw.bytes, raw.bytes + raw.size));
      return true;
    }
    case PD_PAYLOAD_KIND_COUNT:
    default:
      setError(driver, "unknown payload kind");
      return false;
  }
}

}  // namespace

namespace pd {
namespace capi {

pd_printer* attachPrinter(pd_driver* driver, PrinterConfig config) {
  std::shared_ptr<Printer> printer = driver->driver->addPrinter(std::move(config));
  if (!printer) {
    setError(driver, "the core refused the printer configuration");
    return nullptr;
  }
  auto handle = std::unique_ptr<pd_printer>(new pd_printer());
  handle->owner = driver;
  handle->printer = printer;
  handle->id = printer->id();
  pd_printer* raw = handle.get();
  std::lock_guard<std::mutex> lock(driver->mutex);
  driver->printers.push_back(std::move(handle));
  return raw;
}

pd_job* internJob(pd_driver* driver, const std::shared_ptr<PrintJob>& job) {
  if (!job) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(driver->mutex);
  const auto existing = driver->jobs.find(job.get());
  if (existing != driver->jobs.end()) {
    return existing->second.get();
  }
  auto handle = std::unique_ptr<pd_job>(new pd_job());
  handle->owner = driver;
  handle->job = job;
  handle->id = job->id();
  handle->key = job->key();
  pd_job* raw = handle.get();
  driver->jobs.emplace(job.get(), std::move(handle));
  return raw;
}

}  // namespace capi
}  // namespace pd

// --- Driver ------------------------------------------------------------------------

extern "C" pd_driver* pd_create(const pd_config* config) {
  auto driver = std::unique_ptr<pd_driver>(new pd_driver());
  pd::StorageConfig storage;
  if (config != nullptr) {
    if (config->storage_directory != nullptr) {
      storage.directory = config->storage_directory;
    }
    storage.fsync_enabled = config->fsync_disabled == 0;
    driver->log = config->log;
    driver->log_ctx = config->log_ctx;
  }
  try {
    driver->driver.reset(new pd::PrinterDriver(std::move(storage)));
  } catch (const std::exception&) {
    return nullptr;
  }
  return driver.release();
}

extern "C" void pd_destroy(pd_driver* driver) {
  if (driver == nullptr) {
    return;
  }
  // Order matters: the core owns the threads that invoke subscriber callbacks, and
  // those callbacks hold pd_job handles. Stop the threads, drop the core, only then
  // free the handles.
  if (driver->driver) {
    driver->driver->shutdown();
    driver->driver.reset();
  }
  driver->jobs.clear();
  driver->printers.clear();
  delete driver;
}

extern "C" const char* pd_last_error(pd_driver* driver) {
  if (driver == nullptr) {
    return "driver handle is null";
  }
  std::lock_guard<std::mutex> lock(driver->mutex);
  return driver->last_error.c_str();
}

extern "C" const char* const* pd_profile_ids(void) { return kProfileIds; }

// --- Printers ----------------------------------------------------------------------

extern "C" pd_printer* pd_add_printer_tcp(pd_driver* driver, const pd_tcp_config* config) {
  if (driver == nullptr) {
    return nullptr;
  }
  if (config == nullptr || config->host == nullptr || config->host[0] == '\0') {
    setError(driver, "tcp printer needs a host");
    return nullptr;
  }
  const std::string profile_id =
      config->profile_id != nullptr && config->profile_id[0] != '\0' ? config->profile_id
                                                                     : "generic";
  pd::CapabilityProfile profile;
  if (profile_id == "generic") {
    profile = pd::generic_escpos();
  } else if (profile_id == "xp-s260m") {
    profile = pd::xp_s260m();
  } else {
    setError(driver, "unknown profile id: " + profile_id);
    return nullptr;
  }

  pd::PrinterConfig printer;
  if (config->printer_id != nullptr) {
    printer.id = config->printer_id;
  }
  printer.transport = pd::tcp(config->host, config->port != 0 ? config->port : 9100,
                              config->connect_timeout_ms != 0 ? config->connect_timeout_ms
                                                              : 3000);
  printer.width_dots = config->width_dots != 0 ? config->width_dots : pd::escpos::kWidth80mm;
  printer.profile = profile;
  clearError(driver);
  return pd::capi::attachPrinter(driver, std::move(printer));
}

extern "C" const char* pd_printer_id(pd_printer* printer) {
  return printer != nullptr ? printer->id.c_str() : "";
}

extern "C" uint32_t pd_printer_width_dots(pd_printer* printer) {
  return printer != nullptr && printer->printer ? printer->printer->widthDots() : 0;
}

extern "C" pd_completion_mechanism pd_printer_completion(pd_printer* printer) {
  if (printer == nullptr || !printer->printer) {
    return PD_COMPLETION_NONE;
  }
  return static_cast<pd_completion_mechanism>(printer->printer->profile().completion);
}

extern "C" pd_device_status pd_printer_status(pd_driver* driver, pd_printer* printer) {
  if (!checkHandles(driver, printer)) {
    return emptyStatus();
  }
  return toStatus(printer->printer->status());
}

extern "C" pd_device_status pd_printer_refresh_status(pd_driver* driver,
                                                      pd_printer* printer,
                                                      uint32_t timeout_ms) {
  if (!checkHandles(driver, printer)) {
    return emptyStatus();
  }
  return toStatus(printer->printer->refreshStatus(
      std::chrono::milliseconds(timeout_ms != 0 ? timeout_ms : 2000)));
}

extern "C" void pd_open_cash_drawer(pd_driver* driver, pd_printer* printer) {
  if (!checkHandles(driver, printer)) {
    return;
  }
  printer->printer->openCashDrawer();
}

extern "C" void pd_printer_drain(pd_driver* driver, pd_printer* printer) {
  if (!checkHandles(driver, printer)) {
    return;
  }
  printer->printer->drain();
}

extern "C" void pd_subscribe_device(pd_driver* driver, pd_printer* printer,
                                    pd_device_event_cb cb, void* ctx) {
  if (!checkHandles(driver, printer) || cb == nullptr) {
    return;
  }
  printer->printer->subscribe([printer, cb, ctx](pd::DeviceEvent event) {
    cb(printer, static_cast<pd_device_event>(event), ctx);
  });
}

// --- Jobs --------------------------------------------------------------------------

extern "C" pd_job* pd_print(pd_driver* driver, pd_printer* printer,
                            const pd_payload* payload, const pd_job_options* options) {
  if (!checkHandles(driver, printer)) {
    return nullptr;
  }
  pd::Payload built;
  if (!buildPayload(driver, payload, &built)) {
    return nullptr;
  }
  std::shared_ptr<pd::PrintJob> job =
      printer->printer->print(std::move(built), toOptions(options));
  if (!job) {
    setError(driver, "the core did not accept the job");
    return nullptr;
  }
  return pd::capi::internJob(driver, job);
}

extern "C" pd_job* pd_force_reprint(pd_driver* driver, pd_printer* printer,
                                    const char* key, const pd_job_options* options) {
  if (!checkHandles(driver, printer)) {
    return nullptr;
  }
  if (key == nullptr || key[0] == '\0') {
    setError(driver, "force reprint needs the original idempotency key");
    return nullptr;
  }
  pd::JobOptions job_options = toOptions(options);
  job_options.key = key;
  std::shared_ptr<pd::PrintJob> job = printer->printer->forceReprint(key, job_options);
  if (!job) {
    setError(driver,
             "no reprintable job for key " + std::string(key) +
                 " (unknown key, or a job reloaded from the journal, which carries "
                 "state but not bytes)");
    return nullptr;
  }
  return pd::capi::internJob(driver, job);
}

extern "C" pd_job* pd_find_job(pd_driver* driver, const char* key) {
  if (driver == nullptr) {
    return nullptr;
  }
  if (key == nullptr || key[0] == '\0') {
    setError(driver, "find job needs a key");
    return nullptr;
  }
  clearError(driver);
  std::shared_ptr<pd::PrintJob> job = driver->driver->findJob(key);
  if (!job) {
    setError(driver, "no job for key " + std::string(key));
    return nullptr;
  }
  return pd::capi::internJob(driver, job);
}

extern "C" const char* pd_job_id(pd_job* job) {
  return job != nullptr ? job->id.c_str() : "";
}

extern "C" const char* pd_job_key(pd_job* job) {
  return job != nullptr ? job->key.c_str() : "";
}

extern "C" uint32_t pd_job_attempt(pd_job* job) {
  return job != nullptr && job->job ? job->job->attempt() : 0;
}

extern "C" pd_job_state pd_job_current_state(pd_job* job) {
  if (job == nullptr || !job->job) {
    return PD_JOB_STATE_UNKNOWN;
  }
  return static_cast<pd_job_state>(job->job->state());
}

extern "C" pd_confidence_level pd_job_confidence(pd_job* job) {
  if (job == nullptr || !job->job) {
    return PD_CONFIDENCE_TRANSPORT_ACCEPTED;
  }
  return static_cast<pd_confidence_level>(job->job->confidence());
}

extern "C" int32_t pd_job_is_terminal(pd_job* job) {
  return job != nullptr && job->job && job->job->isTerminal() ? 1 : 0;
}

extern "C" void pd_subscribe_job(pd_driver* driver, pd_job* job, pd_job_event_cb cb,
                                 void* ctx) {
  if (!checkJob(driver, job) || cb == nullptr) {
    return;
  }
  job->job->subscribe([job, cb, ctx](const pd::JobEvent& event) {
    const pd_job_event lowered = toEvent(event);
    cb(job, &lowered, ctx);
  });
}

extern "C" int32_t pd_job_await(pd_driver* driver, pd_job* job, uint32_t timeout_ms,
                                pd_job_result* out) {
  if (!checkJob(driver, job)) {
    return 0;
  }
  pd::JobResult result;
  if (timeout_ms == 0) {
    result = job->job->result();
  } else {
    const std::optional<pd::JobResult> settled =
        job->job->result(std::chrono::milliseconds(timeout_ms));
    if (!settled.has_value()) {
      return 0;
    }
    result = *settled;
  }
  if (out != nullptr) {
    out->outcome = static_cast<pd_job_outcome>(result.outcome);
    out->confidence = static_cast<pd_confidence_level>(result.confidence);
    out->reason = static_cast<pd_failure_reason>(result.reason);
  }
  return 1;
}

// --- Enum names --------------------------------------------------------------------

namespace {

template <size_t N>
const char* nameAt(const char* const (&table)[N], int value) {
  return value >= 0 && value < static_cast<int>(N) ? table[value] : "";
}

}  // namespace

extern "C" const char* pd_job_state_name(pd_job_state value) {
  return nameAt(kJobStateNames, value);
}
extern "C" const char* pd_confidence_level_name(pd_confidence_level value) {
  return nameAt(kConfidenceNames, value);
}
extern "C" const char* pd_device_event_name(pd_device_event value) {
  return nameAt(kDeviceEventNames, value);
}
extern "C" const char* pd_failure_reason_name(pd_failure_reason value) {
  return nameAt(kFailureReasonNames, value);
}
extern "C" const char* pd_job_outcome_name(pd_job_outcome value) {
  return nameAt(kJobOutcomeNames, value);
}
extern "C" const char* pd_payload_kind_name(pd_payload_kind value) {
  return nameAt(kPayloadKindNames, value);
}
extern "C" const char* pd_completion_mechanism_name(pd_completion_mechanism value) {
  return nameAt(kCompletionNames, value);
}
extern "C" const char* pd_cut_variant_name(pd_cut_variant value) {
  return nameAt(kCutVariantNames, value);
}

extern "C" pd_code_page pd_code_page_at(int32_t index) {
  if (index < 0 || index >= PD_CODE_PAGE_COUNT) {
    return PD_CODE_PAGE_PC437;
  }
  return kCodePages[index];
}
