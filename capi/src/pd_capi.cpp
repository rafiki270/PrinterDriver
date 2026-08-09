#include "printerdriver/pd.h"

#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "pd_internal.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/device_profiles.hpp"
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
static_assert(PD_DEVICE_FOREIGN_WRITER_DETECTED ==
              value_of(pd::DeviceEvent::ForeignWriterDetected));
static_assert(PD_DEVICE_EVENT_COUNT == static_cast<int>(pd::kAllDeviceEvents.size()));

static_assert(PD_GRADE_APLUS_DURABLE_QUERYABLE_JOB ==
              value_of(pd::ConfidenceGrade::APlus_DurableQueryableJob));
static_assert(PD_GRADE_A_JOB_LEVEL_CONFIRMATION ==
              value_of(pd::ConfidenceGrade::A_JobLevelConfirmation));
static_assert(PD_GRADE_B_ORDERED_DEVICE_RESPONSE ==
              value_of(pd::ConfidenceGrade::B_OrderedDeviceResponse));
static_assert(PD_GRADE_C_DEVICE_STATUS_AROUND ==
              value_of(pd::ConfidenceGrade::C_DeviceStatusAround));
static_assert(PD_GRADE_D_SPOOLER_COMPLETED ==
              value_of(pd::ConfidenceGrade::D_SpoolerCompleted));
static_assert(PD_GRADE_E_TRANSPORT_ONLY == value_of(pd::ConfidenceGrade::E_TransportOnly));
static_assert(PD_GRADE_COUNT == static_cast<int>(pd::kAllConfidenceGrades.size()));

static_assert(PD_PROVENANCE_DOCUMENTED == value_of(pd::Provenance::Documented));
static_assert(PD_PROVENANCE_PROBED == value_of(pd::Provenance::Probed));
static_assert(PD_PROVENANCE_UNVERIFIED == value_of(pd::Provenance::Unverified));
static_assert(PD_PROVENANCE_COUNT == static_cast<int>(pd::kAllProvenances.size()));

static_assert(PD_LANGUAGE_ESC_POS == value_of(pd::CommandLanguage::EscPos));
static_assert(PD_LANGUAGE_STAR_PRNT == value_of(pd::CommandLanguage::StarPrnt));
static_assert(PD_LANGUAGE_STAR_LINE == value_of(pd::CommandLanguage::StarLine));
static_assert(PD_LANGUAGE_EPOS_XML == value_of(pd::CommandLanguage::EposXml));
static_assert(PD_LANGUAGE_ZPL == value_of(pd::CommandLanguage::Zpl));
static_assert(PD_LANGUAGE_CPCL == value_of(pd::CommandLanguage::Cpcl));
static_assert(PD_LANGUAGE_BROTHER_RASTER == value_of(pd::CommandLanguage::BrotherRaster));
static_assert(PD_LANGUAGE_ESC_P == value_of(pd::CommandLanguage::EscP));
static_assert(PD_LANGUAGE_COUNT == static_cast<int>(pd::kAllCommandLanguages.size()));

static_assert(PD_AUTHORITY_PHYSICAL_PRINTER ==
              value_of(pd::CompletionAuthority::PhysicalPrinter));
static_assert(PD_AUTHORITY_VENDOR_SPOOLER ==
              value_of(pd::CompletionAuthority::VendorSpooler));
static_assert(PD_AUTHORITY_PD_AGENT == value_of(pd::CompletionAuthority::PdAgent));
static_assert(PD_AUTHORITY_PRINT_SERVER == value_of(pd::CompletionAuthority::PrintServer));
static_assert(PD_AUTHORITY_TRANSPORT_ONLY ==
              value_of(pd::CompletionAuthority::TransportOnly));
static_assert(PD_AUTHORITY_COUNT ==
              static_cast<int>(pd::kAllCompletionAuthorities.size()));

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
static_assert(PD_COMPLETION_VENDOR_IDLE == value_of(pd::CompletionMechanism::VendorIdle));
static_assert(PD_COMPLETION_EPOS_JOB_ID == value_of(pd::CompletionMechanism::EposJobId));
static_assert(PD_COMPLETION_STAR_CHECKED_BLOCK ==
              value_of(pd::CompletionMechanism::StarCheckedBlock));
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
    "RecoverableError", "UnrecoverableError", "ConnectionLost", "ConnectionRestored",
    "ForeignWriterDetected"};

const char* const kConfidenceGradeNames[PD_GRADE_COUNT] = {
    "APlus_DurableQueryableJob", "A_JobLevelConfirmation", "B_OrderedDeviceResponse",
    "C_DeviceStatusAround",      "D_SpoolerCompleted",     "E_TransportOnly"};

const char* const kConfidenceGradeLetters[PD_GRADE_COUNT] = {"A+", "A", "B",
                                                             "C",  "D", "E"};

const char* const kProvenanceNames[PD_PROVENANCE_COUNT] = {"Documented", "Probed",
                                                           "Unverified"};

const char* const kCommandLanguageNames[PD_LANGUAGE_COUNT] = {
    "EscPos", "StarPrnt", "StarLine",      "EposXml",
    "Zpl",    "Cpcl",     "BrotherRaster", "EscP"};

const char* const kCompletionAuthorityNames[PD_AUTHORITY_COUNT] = {
    "PhysicalPrinter", "VendorSpooler", "PdAgent", "PrintServer", "TransportOnly"};

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

const char* const kCompletionNames[PD_COMPLETION_COUNT] = {
    "GsParenH", "GsR1", "VendorIdle", "EposJobId", "StarCheckedBlock", "None"};

const char* const kCutVariantNames[PD_CUT_VARIANT_COUNT] = {"Partial", "Full", "None"};

const pd_code_page kCodePages[PD_CODE_PAGE_COUNT] = {
    PD_CODE_PAGE_PC437, PD_CODE_PAGE_PC850, PD_CODE_PAGE_WPC1252, PD_CODE_PAGE_PC852,
    PD_CODE_PAGE_PC858};

// The two hand-built profiles of capability_profile.hpp plus every entry in the device
// database (docs/compatibility-brief.md §26). Assembled once, into storage that lives
// forever, because pd.h promises these pointers are valid for the life of the process.
const std::vector<const char*>& profileIds() {
  static const std::vector<std::string>* const owned = [] {
    auto* names = new std::vector<std::string>{"generic", "xp-s260m"};
    for (const std::string& name : pd::devices::names()) {
      names->push_back(name);
    }
    return names;
  }();
  static const std::vector<const char*>* const table = [] {
    auto* ids = new std::vector<const char*>();
    ids->reserve(owned->size() + 1);
    for (const std::string& name : *owned) {
      ids->push_back(name.c_str());
    }
    ids->push_back(nullptr);
    return ids;
  }();
  return *table;
}

// "generic" and "xp-s260m" are the two profiles that predate the database and are kept
// as ids; everything else is looked up by database name. An unknown id is an error and
// not a silent fall back to generic: a caller that asked for a TM-T88VI and got an
// unknown-device profile would be told a weaker completion story than it asked for,
// with nothing in the result explaining why.
bool resolveProfile(const char* profile_id, pd::CapabilityProfile* out,
                    std::string* error) {
  const std::string id =
      profile_id != nullptr && profile_id[0] != '\0' ? profile_id : "generic";
  if (id == "generic") {
    *out = pd::generic_escpos();
    return true;
  }
  if (id == "xp-s260m") {
    *out = pd::xp_s260m();
    return true;
  }
  if (pd::devices::exists(id)) {
    *out = pd::devices::byName(id);
    return true;
  }
  *error = "unknown profile id: " + id;
  return false;
}

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
  // Clamped rather than rejected: a margin wider than the encoder's ESC J chunking can
  // express is a caller asking for more paper than a roll has, and refusing the ticket
  // over presentation whitespace would be the wrong trade.
  const uint32_t kMaxFeedDots = 65535u;
  out.top_feed_dots = static_cast<uint16_t>(
      options->top_feed_dots > kMaxFeedDots ? kMaxFeedDots : options->top_feed_dots);
  out.bottom_feed_dots = static_cast<uint16_t>(
      options->bottom_feed_dots > kMaxFeedDots ? kMaxFeedDots : options->bottom_feed_dots);
  out.print_verification_id = options->suppress_verification_id == 0;
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

extern "C" const char* const* pd_profile_ids(void) { return profileIds().data(); }

// --- Printers ----------------------------------------------------------------------

extern "C" pd_printer* pd_add_printer_tcp(pd_driver* driver, const pd_tcp_config* config) {
  if (driver == nullptr) {
    return nullptr;
  }
  if (config == nullptr || config->host == nullptr || config->host[0] == '\0') {
    setError(driver, "tcp printer needs a host");
    return nullptr;
  }
  pd::CapabilityProfile profile;
  std::string error;
  if (!resolveProfile(config->profile_id, &profile, &error)) {
    setError(driver, error);
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

extern "C" pd_printer* pd_add_printer_custom(pd_driver* driver,
                                             const pd_transport_vtable* vtable,
                                             void* ctx, const char* profile_id,
                                             uint32_t width_dots) {
  if (driver == nullptr) {
    return nullptr;
  }
  if (vtable == nullptr || vtable->connect == nullptr || vtable->write == nullptr) {
    setError(driver, "custom transport needs at least connect and write");
    return nullptr;
  }
  pd::CapabilityProfile profile;
  std::string error;
  if (!resolveProfile(profile_id, &profile, &error)) {
    setError(driver, error);
    return nullptr;
  }

  // The function pointers are copied out of the caller's struct here, as pd.h promises;
  // only `ctx` and the pointed-to functions have to outlive the call.
  pd::CustomTransportLink::Callbacks callbacks;
  const pd_transport_connect_fn on_connect = vtable->connect;
  const pd_transport_write_fn on_write = vtable->write;
  const pd_transport_close_fn on_close = vtable->close;
  callbacks.connect = [on_connect, ctx]() { return on_connect(ctx) != 0; };
  callbacks.write = [on_write, ctx](const uint8_t* data, size_t size) {
    return on_write(ctx, data, size);
  };
  if (on_close != nullptr) {
    callbacks.close = [on_close, ctx]() { on_close(ctx); };
  }
  callbacks.description =
      vtable->description != nullptr && vtable->description[0] != '\0'
          ? vtable->description
          : "custom";

  auto link = std::make_shared<pd::CustomTransportLink>(std::move(callbacks));

  pd::PrinterConfig printer;
  printer.transport = pd::customTransport(link);
  printer.width_dots = width_dots != 0 ? width_dots : pd::escpos::kWidth80mm;
  printer.profile = profile;
  clearError(driver);
  pd_printer* handle = pd::capi::attachPrinter(driver, std::move(printer));
  if (handle != nullptr) {
    handle->link = std::move(link);
  }
  return handle;
}

extern "C" int32_t pd_transport_feed_bytes(pd_printer* printer, const uint8_t* data,
                                           size_t size) {
  if (printer == nullptr || !printer->link || data == nullptr || size == 0) {
    return 0;
  }
  return printer->link->feedBytes(data, size) ? 1 : 0;
}

extern "C" int32_t pd_transport_link_dropped(pd_printer* printer, const char* message) {
  if (printer == nullptr || !printer->link) {
    return 0;
  }
  return printer->link->linkDropped(message != nullptr ? message : "link dropped") ? 1
                                                                                  : 0;
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

extern "C" pd_provenance pd_printer_completion_provenance(pd_printer* printer) {
  if (printer == nullptr || !printer->printer) {
    return PD_PROVENANCE_UNVERIFIED;
  }
  const pd::CapabilityProfile profile = printer->printer->profile();
  // The provenance of the capability the fence actually rests on, not a summary of the
  // whole profile: a printer whose GS ( H is documented but whose GS r has only ever
  // been assumed is answering two different questions, and only one of them is being
  // asked here.
  switch (profile.completion) {
    case pd::CompletionMechanism::GsParenH:
      return static_cast<pd_provenance>(
          profile.completion_caps.process_id_gs_h_provenance);
    case pd::CompletionMechanism::GsR1:
      return static_cast<pd_provenance>(profile.completion_caps.queued_gs_r_provenance);
    case pd::CompletionMechanism::VendorIdle:
    case pd::CompletionMechanism::StarCheckedBlock:
      return static_cast<pd_provenance>(profile.completion_caps.vendor_idle_provenance);
    case pd::CompletionMechanism::EposJobId:
      return static_cast<pd_provenance>(profile.completion_caps.epos_job_id_provenance);
    case pd::CompletionMechanism::None:
      break;
  }
  // No fence, so there is nothing to have documented or probed.
  return PD_PROVENANCE_UNVERIFIED;
}

extern "C" pd_command_language pd_printer_language(pd_printer* printer) {
  if (printer == nullptr || !printer->printer) {
    return PD_LANGUAGE_ESC_POS;
  }
  return static_cast<pd_command_language>(printer->printer->profile().language);
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
  pd_reprint_options reprint;
  std::memset(&reprint, 0, sizeof(reprint));
  if (options != nullptr) {
    reprint.job = *options;
  }
  return pd_force_reprint_opts(driver, printer, key, &reprint);
}

extern "C" pd_job* pd_force_reprint_opts(pd_driver* driver, pd_printer* printer,
                                         const char* key,
                                         const pd_reprint_options* options) {
  if (!checkHandles(driver, printer)) {
    return nullptr;
  }
  if (key == nullptr || key[0] == '\0') {
    setError(driver, "force reprint needs the original idempotency key");
    return nullptr;
  }
  pd::ReprintOptions reprint;
  if (options != nullptr) {
    reprint.job = toOptions(&options->job);
    reprint.banner = options->suppress_banner == 0;
  }
  reprint.job.key = key;
  std::shared_ptr<pd::PrintJob> job = printer->printer->forceReprint(key, reprint);
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

extern "C" pd_job* pd_job_by_token(pd_driver* driver, const char* token) {
  if (driver == nullptr) {
    return nullptr;
  }
  if (token == nullptr || token[0] == '\0') {
    setError(driver, "job lookup needs a verification token");
    return nullptr;
  }
  clearError(driver);
  std::shared_ptr<pd::PrintJob> job = driver->driver->jobByToken(token);
  if (!job) {
    setError(driver, "no job for verification token " + std::string(token));
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

namespace {

// The tokens are minted on the submitting thread but a handle can be interned before
// they are set, so they are cached on first read rather than at intern time. Under the
// driver lock: the string a previous call returned must not be reallocated underneath
// a caller still holding the pointer, which is why it is only ever written once.
const char* cachedToken(pd_job* job, std::string pd_job::*slot,
                        std::string (pd::PrintJob::*read)() const) {
  if (job == nullptr || !job->job || job->owner == nullptr) {
    return "";
  }
  std::lock_guard<std::mutex> lock(job->owner->mutex);
  std::string& cached = job->*slot;
  if (cached.empty()) {
    cached = ((*job->job).*read)();
  }
  return cached.c_str();
}

}  // namespace

extern "C" const char* pd_job_print_token(pd_job* job) {
  return cachedToken(job, &pd_job::print_token, &pd::PrintJob::printToken);
}

extern "C" const char* pd_job_cut_token(pd_job* job) {
  return cachedToken(job, &pd_job::cut_token, &pd::PrintJob::cutToken);
}

extern "C" const char* pd_instance_nonce(pd_driver* driver) {
  if (driver == nullptr || !driver->driver) {
    return "";
  }
  return driver->driver->instanceNonce().c_str();
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
  // By value: the callback may outlive this frame (pd.h, pd_job_event_cb), which is
  // what lets an async FFI consumer see transitions live instead of replaying them
  // after the job settles.
  job->job->subscribe(
      [job, cb, ctx](const pd::JobEvent& event) { cb(job, toEvent(event), ctx); });
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
    out->grade = static_cast<pd_confidence_grade>(result.grade);
    out->authority = static_cast<pd_completion_authority>(result.authority);
    // The method string belongs to the handle, not to this frame: a caller keeping the
    // pd_job_result must still be able to read it. A terminal job's result never
    // changes, so writing it once per await is idempotent.
    {
      std::lock_guard<std::mutex> lock(driver->mutex);
      job->method = result.method;
      out->method = job->method.c_str();
    }
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
extern "C" const char* pd_confidence_grade_name(pd_confidence_grade value) {
  return nameAt(kConfidenceGradeNames, value);
}
extern "C" const char* pd_completion_authority_name(pd_completion_authority value) {
  return nameAt(kCompletionAuthorityNames, value);
}
extern "C" const char* pd_provenance_name(pd_provenance value) {
  return nameAt(kProvenanceNames, value);
}
extern "C" const char* pd_command_language_name(pd_command_language value) {
  return nameAt(kCommandLanguageNames, value);
}
extern "C" const char* pd_confidence_grade_letter(pd_confidence_grade value) {
  return nameAt(kConfidenceGradeLetters, value);
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

// ====================================================================================
// M14 — CASH DRAWER (docs/cash-drawer.md)
// ====================================================================================

static_assert(PD_DRAWER_CLOSED == value_of(pd::DrawerState::Closed));
static_assert(PD_DRAWER_OPEN == value_of(pd::DrawerState::Open));
static_assert(PD_DRAWER_OPENING == value_of(pd::DrawerState::Opening));
static_assert(PD_DRAWER_KICK_SENT_UNVERIFIED ==
              value_of(pd::DrawerState::KickSentUnverified));
static_assert(PD_DRAWER_OPEN_VERIFIED == value_of(pd::DrawerState::OpenVerified));
static_assert(PD_DRAWER_FAILED_TO_OPEN == value_of(pd::DrawerState::FailedToOpen));
static_assert(PD_DRAWER_NO_SENSOR == value_of(pd::DrawerState::NoSensor));
static_assert(PD_DRAWER_UNKNOWN == value_of(pd::DrawerState::Unknown));
static_assert(PD_DRAWER_STATE_COUNT == static_cast<int>(pd::kAllDrawerStates.size()));

static_assert(PD_DRAWER_PORT_EPSON_24V_6P6C ==
              value_of(pd::DrawerPortStandard::Epson24V6P6C));
static_assert(PD_DRAWER_PORT_STAR_24V_6P6C ==
              value_of(pd::DrawerPortStandard::Star24V6P6C));
static_assert(PD_DRAWER_PORT_GENERIC_12V_6P6C ==
              value_of(pd::DrawerPortStandard::Generic12V6P6C));
static_assert(PD_DRAWER_PORT_UNKNOWN == value_of(pd::DrawerPortStandard::Unknown));
static_assert(PD_DRAWER_PORT_STANDARD_COUNT ==
              static_cast<int>(pd::kAllDrawerPortStandards.size()));

static_assert(PD_DRAWER_KICK_EPSON_ESC_P == value_of(pd::DrawerKickMethod::EpsonEscP));
static_assert(PD_DRAWER_KICK_EPSON_EPOS == value_of(pd::DrawerKickMethod::EpsonEpos));
static_assert(PD_DRAWER_KICK_STAR_PRNT == value_of(pd::DrawerKickMethod::StarPrnt));
static_assert(PD_DRAWER_KICK_BIXOLON_SDK == value_of(pd::DrawerKickMethod::BixolonSdk));
static_assert(PD_DRAWER_KICK_CITIZEN_ESC_P ==
              value_of(pd::DrawerKickMethod::CitizenEscP));
static_assert(PD_DRAWER_KICK_SNBC_ESC_P == value_of(pd::DrawerKickMethod::SnbcEscP));
static_assert(PD_DRAWER_KICK_VENDOR == value_of(pd::DrawerKickMethod::Vendor));
static_assert(PD_DRAWER_KICK_UNSUPPORTED == value_of(pd::DrawerKickMethod::Unsupported));
static_assert(PD_DRAWER_KICK_METHOD_COUNT ==
              static_cast<int>(pd::kAllDrawerKickMethods.size()));

static_assert(PD_DRAWER_STATUS_GS_R2 == value_of(pd::DrawerStatusMethod::GsR2));
static_assert(PD_DRAWER_STATUS_ASB == value_of(pd::DrawerStatusMethod::Asb));
static_assert(PD_DRAWER_STATUS_STAR_SIGNAL ==
              value_of(pd::DrawerStatusMethod::StarSignal));
static_assert(PD_DRAWER_STATUS_VENDOR_SDK == value_of(pd::DrawerStatusMethod::VendorSdk));
static_assert(PD_DRAWER_STATUS_NONE == value_of(pd::DrawerStatusMethod::None));
static_assert(PD_DRAWER_STATUS_METHOD_COUNT ==
              static_cast<int>(pd::kAllDrawerStatusMethods.size()));

namespace {

const char* const kDrawerStateNames[PD_DRAWER_STATE_COUNT] = {
    "Closed", "Open", "Opening", "KickSentUnverified",
    "OpenVerified", "FailedToOpen", "NoSensor", "Unknown"};

const char* const kDrawerPortStandardNames[PD_DRAWER_PORT_STANDARD_COUNT] = {
    "Epson24V6P6C", "Star24V6P6C", "Generic12V6P6C", "Unknown"};

const char* const kDrawerKickMethodNames[PD_DRAWER_KICK_METHOD_COUNT] = {
    "EpsonEscP", "EpsonEpos", "StarPrnt",  "BixolonSdk",
    "CitizenEscP", "SnbcEscP", "Vendor",   "Unsupported"};

const char* const kDrawerStatusMethodNames[PD_DRAWER_STATUS_METHOD_COUNT] = {
    "GsR2", "Asb", "StarSignal", "VendorSdk", "None"};

pd_drawer_capabilities toDrawerCapabilities(const pd::DrawerCapabilities& caps) {
  pd_drawer_capabilities out{};
  out.present = caps.present ? PD_TRUE : PD_FALSE;
  out.standard = static_cast<pd_drawer_port_standard>(caps.electrical.standard);
  out.voltage = caps.electrical.voltage;
  out.max_current_ma = caps.electrical.max_current_ma;
  out.channel_count = caps.electrical.channel_count;
  out.sensor_pin = caps.electrical.sensor_pin;
  out.method = static_cast<pd_drawer_kick_method>(caps.kick.method);
  out.default_pulse_ms = caps.kick.default_pulse_ms;
  out.max_pulse_ms = caps.kick.max_pulse_ms;
  out.cooldown_ms = caps.kick.cooldown_ms;
  out.can_kick_during_print = caps.kick.can_kick_during_print ? PD_TRUE : PD_FALSE;
  out.status_available = caps.status.available ? PD_TRUE : PD_FALSE;
  out.status_method = static_cast<pd_drawer_status_method>(caps.status.method);
  out.shared_between_drawers = caps.status.shared_between_drawers ? PD_TRUE : PD_FALSE;
  out.shared_with_buzzer = caps.port.shared_with_buzzer ? PD_TRUE : PD_FALSE;
  out.electrical_provenance = static_cast<pd_provenance>(caps.evidence.electrical);
  out.commands_provenance = static_cast<pd_provenance>(caps.evidence.commands);
  out.kickable = caps.kickable() ? PD_TRUE : PD_FALSE;
  return out;
}

// A drawer facet nobody could look at: no port, unsupported method, unclassified
// standard. Every zero here is the conservative answer, which is why an all-zeroes
// struct is a safe one.
pd_drawer_capabilities emptyDrawerCapabilities() {
  pd_drawer_capabilities out{};
  out.standard = PD_DRAWER_PORT_UNKNOWN;
  out.method = PD_DRAWER_KICK_UNSUPPORTED;
  out.status_method = PD_DRAWER_STATUS_NONE;
  out.electrical_provenance = PD_PROVENANCE_UNVERIFIED;
  out.commands_provenance = PD_PROVENANCE_UNVERIFIED;
  return out;
}

pd_drawer_result toDrawerResult(const pd::DrawerOpenResult& result) {
  pd_drawer_result out{};
  out.state = static_cast<pd_drawer_state>(result.state);
  out.previous_state = static_cast<pd_drawer_state>(result.previous_state);
  out.channel = result.channel;
  out.pulse_ms = result.pulse_ms;
  out.elapsed_ms = result.elapsed_ms;
  return out;
}

pd_drawer_reading toDrawerReading(const pd::DrawerReading& reading) {
  pd_drawer_reading out{};
  out.available = reading.available ? PD_TRUE : PD_FALSE;
  out.answered = reading.answered ? PD_TRUE : PD_FALSE;
  out.pin_high = reading.pin_high.has_value() ? (*reading.pin_high ? PD_TRUE : PD_FALSE)
                                              : PD_UNKNOWN;
  out.needs_calibration = reading.needs_calibration ? PD_TRUE : PD_FALSE;
  out.state = static_cast<pd_drawer_state>(reading.state);
  return out;
}

}  // namespace

extern "C" pd_drawer_capabilities pd_printer_drawer_capabilities(pd_printer* printer) {
  if (printer == nullptr || !printer->printer) {
    return emptyDrawerCapabilities();
  }
  return toDrawerCapabilities(printer->printer->profile().drawer);
}

extern "C" pd_drawer_result pd_drawer_open(pd_driver* driver, pd_printer* printer,
                                           const pd_drawer_request* request) {
  if (!checkHandles(driver, printer)) {
    pd_drawer_result out{};
    out.state = PD_DRAWER_UNKNOWN;
    out.previous_state = PD_DRAWER_UNKNOWN;
    out.channel = 1;
    return out;
  }
  pd::DrawerRequest wanted;
  if (request != nullptr) {
    wanted.channel = request->channel;
    wanted.pulse_ms = request->pulse_ms;
  }
  return toDrawerResult(printer->printer->openDrawer(wanted));
}

extern "C" pd_drawer_reading pd_drawer_read_sensor(pd_driver* driver, pd_printer* printer,
                                                   uint32_t timeout_ms) {
  if (!checkHandles(driver, printer)) {
    pd_drawer_reading out{};
    out.pin_high = PD_UNKNOWN;
    out.needs_calibration = PD_TRUE;
    out.state = PD_DRAWER_UNKNOWN;
    return out;
  }
  return toDrawerReading(printer->printer->readDrawerSensor(
      std::chrono::milliseconds(timeout_ms != 0 ? timeout_ms : 1500)));
}

extern "C" int32_t pd_drawer_calibrate_polarity(pd_driver* driver, pd_printer* printer,
                                                int32_t high_means_open) {
  if (!checkHandles(driver, printer)) {
    return 0;
  }
  return printer->printer->calibrateDrawerPolarity(high_means_open != 0) ? 1 : 0;
}

extern "C" int32_t pd_drawer_polarity_calibrated(pd_driver* driver, pd_printer* printer) {
  if (!checkHandles(driver, printer)) {
    return 0;
  }
  return printer->printer->drawerPolarity().calibrated ? 1 : 0;
}

extern "C" int32_t pd_drawer_high_means_open(pd_driver* driver, pd_printer* printer) {
  if (!checkHandles(driver, printer)) {
    return 0;
  }
  return printer->printer->drawerPolarity().high_means_open ? 1 : 0;
}

extern "C" const char* pd_drawer_state_name(pd_drawer_state value) {
  return nameAt(kDrawerStateNames, value);
}
extern "C" const char* pd_drawer_port_standard_name(pd_drawer_port_standard value) {
  return nameAt(kDrawerPortStandardNames, value);
}
extern "C" const char* pd_drawer_kick_method_name(pd_drawer_kick_method value) {
  return nameAt(kDrawerKickMethodNames, value);
}
extern "C" const char* pd_drawer_status_method_name(pd_drawer_status_method value) {
  return nameAt(kDrawerStatusMethodNames, value);
}

// ================================ end M14 ==========================================
