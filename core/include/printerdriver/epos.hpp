#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "printerdriver/capability_profile.hpp"
#include "printerdriver/types.hpp"

// M13b. The Epson ePOS-Print service (docs/wire-protocols.md §1).
//
// This is the only mechanism in the whole SDK that survives the application losing its
// connection between submitting a receipt and learning what happened to it, and that is
// the entire reason it exists as a separate transport rather than as another fence
// bolted onto the raw byte path. A raw ESC/POS fence is a promise about *this socket*: if
// the process dies while a GS ( H echo is in flight, the echo is gone and the job is
// permanently Unknown. An ePOS job has a durable printer-side identity — a JobID the
// spooler keeps — so a completely different process, minutes later, can ask "what
// happened to ABC123?" and get an answer. docs/compatibility-brief.md §24 puts that at
// the top of the confidence hierarchy, and it is why this is the one grade A+ path here.
//
// -- The discipline this file exists to enforce ---------------------------------------
//
// On a printer with the spooler enabled, the response to a submission is an **enqueue
// acknowledgement, not a print**. It carries `success="true"` and a status of 0x00000002
// ("printing completed") or 0x80000000 ("spooler stopped") *before any paper has moved*.
// Reading that first response as completion is the single most damaging mistake available
// on this protocol: it produces a confident, grade-A+ "printed" for a receipt that is
// still queued behind a cover-open fault. So the client below treats the first
// success=true on a spooler printer as **accepted-not-printed** and refuses to terminate
// the job until a poll returns a genuinely terminal answer.
//
// On a printer *without* the spooler the semantics are the opposite — the submission does
// not return until the data has printed — so the same response is real completion. Same
// bytes, same status field, opposite meaning, decided by a per-model profile flag. Which
// is why EposCapabilities::spooler exists and why it is documented rather than guessed:
// "OmniLink" in a product name is not a capability proxy.
//
// -- Authentication --------------------------------------------------------------------
//
// The print service documents none, so this client sends none. It must never reach for
// WebConfig credentials (the legacy Digest epson/epson, or the newer epson + serial):
// those are administrative credentials for a different service, and an SDK that
// speculatively tries them is an SDK that locks out accounts and logs passwords.

namespace pd::epos {

// --- Response codes (docs/wire-protocols.md §1) --------------------------------------
//
// The documented set, closed here because the client has to *decide* on each one:
// whether it is terminal, and what a caller should be told. An unrecognised string is
// Unrecognised rather than silently a failure — a firmware that adds a code must produce
// an honest "we do not know what this means", not a fabricated verdict.
enum class ResponseCode {
  None,  // empty code attribute, which is what a plain success carries

  // Device faults.
  EPTR_AUTOMATICAL,   // recoverable, printer is auto-recovering
  EPTR_BATTERY_LOW,
  EPTR_COVER_OPEN,
  EPTR_CUTTER,
  EPTR_MECHANICAL,
  EPTR_REC_EMPTY,     // roll end
  EPTR_UNRECOVERABLE,
  ERROR_WAIT_EJECT,

  // Request/service faults.
  SchemaError,
  DeviceNotFound,
  PrintSystemError,
  EX_BADPORT,
  EX_TIMEOUT,
  EX_SPOOLER,             // the spooler's queue is full
  TooManyRequests,
  RequestEntityTooLarge,

  // Job lifecycle.
  JobNotFound,
  Printing,      // NON-TERMINAL
  JobSpooling,   // NON-TERMINAL

  Unrecognised,
};

const char* to_string(ResponseCode) noexcept;
// Parses the `code` attribute. Matching is exact and case-sensitive, as the service
// spells them.
ResponseCode responseCodeFrom(std::string_view text) noexcept;
// Printing and JobSpooling only. Everything else ends the wait, one way or another.
bool isNonTerminal(ResponseCode) noexcept;

// --- Status mask (docs/wire-protocols.md §1) ------------------------------------------
//
// A 32-bit unsigned field. It is unsigned in the wire format and it is unsigned here,
// because the top bit is used: 0x80000000 does not fit in a signed 32-bit integer, and a
// parser that reaches for a signed type reports spooler-stopped as a negative number or
// as garbage. The doc calls this out for JavaScript specifically; the same trap exists in
// any language whose "integer" is signed by default.
namespace status {
inline constexpr uint32_t kNoResponse = 0x00000001u;
inline constexpr uint32_t kPrintingCompleted = 0x00000002u;
inline constexpr uint32_t kDrawerOrBatteryOffline = 0x00000004u;
inline constexpr uint32_t kOffline = 0x00000008u;
inline constexpr uint32_t kCoverOpen = 0x00000020u;
inline constexpr uint32_t kFeedSwitchFeeding = 0x00000040u;
inline constexpr uint32_t kWaitingOnlineRecovery = 0x00000100u;
inline constexpr uint32_t kPanelFeedHeld = 0x00000200u;
inline constexpr uint32_t kMechanicalError = 0x00000400u;
inline constexpr uint32_t kCutterError = 0x00000800u;
inline constexpr uint32_t kUnrecoverableError = 0x00002000u;
inline constexpr uint32_t kAutoRecoveryError = 0x00004000u;
inline constexpr uint32_t kWaitSlipInsertion = 0x00010000u;
inline constexpr uint32_t kRollNearEnd = 0x00020000u;
inline constexpr uint32_t kWaitSlipEjection = 0x00040000u;
inline constexpr uint32_t kPaperEnd = 0x00080000u;
// Model-dependent: buzzer, or label/paper removal waiting.
inline constexpr uint32_t kBuzzerOrRemovalWait = 0x01000000u;
inline constexpr uint32_t kNoPaperAtPeeler = 0x04000000u;
inline constexpr uint32_t kSpoolerStopped = 0x80000000u;
}  // namespace status

// Lowers the mask to the SDK's closed per-printer event enum. Only bits the mask
// actually carries produce events.
std::vector<DeviceEvent> toDeviceEvents(uint32_t status);

// --- Parsed response -------------------------------------------------------------------

struct Response {
  bool parsed = false;      // a <response> element in the ePOS-Print namespace was found
  bool success = false;
  ResponseCode code = ResponseCode::None;
  std::string code_text;    // as received, including codes this build does not know
  uint32_t status = 0;
  uint32_t battery = 0;
  std::string printjobid;   // echoed back by services that carry it
};

// Parses a SOAP envelope and extracts the ePOS-Print <response>.
//
// Matched by **XML namespace plus local name**, never by the `s:` prefix. The prefix is
// chosen by whoever wrote the sender: Epson's own examples use `s:`, other stacks use
// `soap:` or `SOAP-ENV:` or no prefix at all with a default xmlns, and every one of them
// is correct XML. A parser that greps for "s:Envelope" works on the sample in the manual
// and fails on a real device behind a middlebox that re-serialised the document.
bool parseResponse(std::string_view xml, Response* out);

// --- Request building --------------------------------------------------------------

// 1-30 characters of alphanumerics plus `_`, `-` and `.`. Enforced rather than trusted:
// an out-of-range JobID is rejected by the service *after* the receipt has been sent on
// some firmware, which would leave a printed job with no retrievable identity — the exact
// failure this whole transport exists to remove.
bool isValidJobId(std::string_view job_id) noexcept;

// Escapes the five XML metacharacters. Text goes into the document as character data, so
// an order key containing `&` must not be able to break the envelope.
std::string escapeXml(std::string_view text);

// A minimal <epos-print> body builder: the receipt subset of the ePOS-Print XML schema.
//
// Deliberately small. This is not a second receipt renderer — the DSL already is one —
// it is the ePOS spelling of "text, feed, cut", which is what a fenced receipt needs.
// Everything absent from it is listed in declaredDegradations().
class Document {
 public:
  Document& text(std::string_view utf8);          // <text>
  Document& line(std::string_view utf8 = {});     // <text> with a trailing newline
  Document& align(escpos::Alignment alignment);   // <text align="...">
  Document& bold(bool enabled);                   // <text em="true|false">
  Document& feed(uint32_t lines);                 // <feed line="n"/>
  Document& cut(bool full = false);               // <cut type="feed|no_feed"/>
  Document& raw(std::string_view xml);            // escape hatch, inserted verbatim

  const std::string& body() const noexcept { return body_; }
  bool empty() const noexcept { return body_.empty(); }

 private:
  std::string body_;
};

// The full spooled print request of docs/wire-protocols.md §1: SOAP header carrying
// devid, timeout and printjobid; SOAP body carrying <epos-print> with `document` inside.
std::string buildPrintEnvelope(const std::string& device_id, uint32_t timeout_ms,
                               const std::string& job_id, const std::string& document);

// The retrieval request: the same envelope with **only** <printjobid> in the header and
// an **empty** <epos-print/> in the body. That emptiness is the entire protocol for
// "tell me about this job" — there is no separate query verb.
std::string buildPollEnvelope(const std::string& job_id);

// --- Client ----------------------------------------------------------------------------

struct HttpReply {
  bool ok = false;
  std::string error;
  int status = 0;
  std::string body;
};

// One blocking POST. The default implementation is plain BSD/Winsock sockets through
// printerdriver/net_platform.hpp — no HTTP library, because the core ships with no
// dependencies at all and the request shape here is fixed and tiny. Replaceable so a
// suite can drive the state machine without a socket; the suite that ships uses a real
// loopback server anyway, because a protocol client that never crossed a socket proves
// nothing about the socket.
using HttpPost = std::function<HttpReply(const std::string& path, const std::string& body,
                                         uint32_t timeout_ms)>;

struct ClientConfig {
  std::string host;
  uint16_t port = 80;
  // The documented endpoint. Carried as configuration because the older, pre-WSDL form
  // puts devid and timeout in the query string of the same path.
  std::string path = "/cgi-bin/epos/service.cgi";
  std::string device_id = "local_printer";
  // Service-side print timeout. Default 60 000 ms, capped at 300 000 by the service.
  uint32_t timeout_ms = 60000;
  // Whether the printer's spooler is enabled — the per-model fact from the documented
  // matrix. It decides whether the first success=true is completion or an enqueue
  // acknowledgement, so it is the single most consequential field in this struct.
  bool spooler = false;

  uint32_t connect_timeout_ms = 3000;
  uint32_t http_timeout_ms = 20000;
  // How often to retrieve the result of a spooled job, and for how long in total. The
  // total is a budget on *our* patience, not on the printer's: running out produces
  // Unknown, never Failed, because a job we stopped asking about may still be printing.
  uint32_t poll_interval_ms = 500;
  uint32_t poll_budget_ms = 60000;
};

// What one submit-and-retrieve produced.
struct Outcome {
  // False while the service is still working: `Printing`, `JobSpooling`, and the
  // enqueue acknowledgement itself. A non-terminal Outcome is not an answer and must
  // never be published as one — it is the caller's signal to keep retrieving.
  bool terminal = true;
  JobResult result;
  // The terminal state a job on this transport would publish. DoneSoftware on success,
  // FailedKnown on a device or request fault, Unknown when the answer never settled.
  JobState state = JobState::Unknown;
  // True when the submission was acknowledged by a spooler and the paper had provably
  // not moved yet. It stays true on the Outcome as a record of how the job began, which
  // is what distinguishes "the spooler took it and then told us it printed" from "the
  // printer answered when it had finished", even though both end grade A+/A.
  bool accepted_not_printed = false;
  uint32_t polls = 0;
  Response submission;   // the first answer
  Response last;         // the answer that ended the wait (== submission when immediate)
  std::vector<DeviceEvent> events;  // every bit every response carried, in order
  std::string job_id;
  std::string error;     // transport-level failure text, empty when the service answered
};

class Client {
 public:
  explicit Client(ClientConfig config);

  const ClientConfig& config() const noexcept { return config_; }

  // Replaces the socket layer. Must be set before any call; not synchronised.
  void setTransport(HttpPost post);

  // Submits `document` under `job_id` and, on a spooler printer, retrieves the result
  // until it is terminal or the budget runs out.
  Outcome print(const std::string& document, const std::string& job_id);

  // The two halves, exposed because they are separately meaningful: a process that
  // crashed after submitting can come back and call retrieve() with the JobID it
  // journaled, which is the recovery property that makes this grade A+ in the first
  // place.
  Response submit(const std::string& document, const std::string& job_id,
                  std::string* error);
  Response retrieve(const std::string& job_id, std::string* error);

  // Applies the terminal-state rules to one response, in the caller's own loop.
  static Outcome classify(const Response& response, bool spooler, bool first);

 private:
  HttpReply post(const std::string& body);

  ClientConfig config_;
  HttpPost post_;
};

// The ePOS document subset this client does not build, stated rather than implied.
const std::vector<std::string>& declaredDegradations();

}  // namespace pd::epos
