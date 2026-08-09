#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "printerdriver/agent/http.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/dsl/json.hpp"
#include "printerdriver/types.hpp"

// A CloudPRNT server embedded in pd-agent (docs/wire-protocols.md §2, "CloudPRNT").
//
// -- Why this is not a transport ----------------------------------------------------
//
// Every other printer in this codebase is *pushed* to: the engine opens the socket,
// writes the receipt, and waits for a fence on that same session. A CloudPRNT printer
// does the opposite — it polls a URL, downloads the job itself, and says afterwards how
// it went. There is no session to hold, nothing to write into, and no moment at which
// the engine could observe a fence. So a CloudPRNT printer is deliberately **not** a
// pd::Printer and never goes through pd::PrinterDriver's byte transport: wiring it there
// would mean manufacturing a send, and then manufacturing the fence that send never had.
//
// The job repository therefore lives here. What it borrows from the core is the
// vocabulary — pd::JobResult, pd::JobEvidence, pd::DeviceEvent — so that a claim made
// on this path is graded on exactly the same scale as a claim made by the engine
// (docs/compatibility-brief.md §24) and the agent's reporting stays one document shape.
//
// -- The contract -------------------------------------------------------------------
//
// Three requests, all issued *by the printer*, all against its configured CloudPRNT URL
// (docs/wire-protocols.md §2). The server never pushes and never initiates:
//
//   POST   {status, statusCode, printerMAC, uniqueID, jobToken, printingInProgress}
//          → {jobReady, mediaTypes[], jobToken, deleteMethod:"DELETE",
//             jobGetUrl?, jobConfirmationUrl?}
//   GET    ?uid&type&mac&token            → the job bytes, as the negotiated media type
//   DELETE ?uid&mac&code=<status>&token   → the confirmation, and the only delete
//
// and the document's three server rules, which are the whole reason this file is not a
// hash map:
//
//   1. **Retain the job until it is confirmed.** A printer whose transfer was cut off
//      comes back and asks for the same token again.
//   2. **Key by printer identity + token.** A job belongs to one printer and one token;
//      no other printer may download it or confirm it.
//   3. **Never consume on GET alone.** The download is idempotent — the same token
//      yields the same bytes as often as the printer asks — and only the DELETE retires
//      the job.
//
// -- What a confirmation is worth ----------------------------------------------------
//
// `code=200` is a job-level statement by the mechanism that moved the paper, so it is
// graded exactly like a GS ( H echo: Done, `A_JobLevelConfirmation`, `PhysicalPrinter`,
// method `"CloudPRNT"` (docs/compatibility-brief.md §24, "A — explicit device
// completion"). Everything else is not that. A documented 4xx/5xx code becomes an honest
// failure carrying the documented reason; an undocumented code becomes Unknown; and a
// job that was downloaded but never confirmed stays **non-terminal** for as long as that
// is true, because a poll says only that the printer is alive — never that a receipt
// exists. Nothing here upgrades a claim, and nothing here invents one out of silence.

namespace pd::agent {

// The method string on every piece of evidence this path produces. It names the
// mechanism a support engineer has to look up six months later, exactly like
// "GS(H) fn48" does for the pushed path.
inline constexpr const char* kCloudPrntMethod = "CloudPRNT";

// Star's own line-mode media type: what a TSP100IV-family printer asks for unless it was
// configured otherwise. It is only a default — the advertised list is per printer.
inline constexpr const char* kCloudPrntDefaultMediaType = "application/vnd.star.line";

// One CloudPRNT printer this agent serves. There is no host and no port: the printer
// dials us, so all we can know about it is which URL it polls and which MAC it claims.
struct CloudPrntSpec {
  // Route segment: the printer's CloudPRNT URL is `http://<agent>/cloudprnt/<id>`.
  std::string id;
  // Pinned identity (rule 2). Empty means trust-on-first-use: the first MAC to poll the
  // route is adopted and enforced from then on. Pinning it in the config removes that
  // window, which matters on a segment where anything can send an HTTP POST.
  std::string mac;
  // What the poll answer advertises, and the only types a job on this printer may carry.
  // Empty is filled with kCloudPrntDefaultMediaType when the printer is added.
  std::vector<std::string> media_types;
  // Bounded on purpose. A printer that stopped polling must not turn an unbounded queue
  // into the agent's memory problem — refusing loudly beats accumulating (the same rule
  // the HTTP server applies to accepted connections).
  size_t max_pending = 32;
  // How many confirmed jobs keep their result after the bytes are dropped. This is what
  // lets a retried DELETE (`retry=x` is documented) answer success instead of 404.
  size_t max_history = 256;
};

// What one of the documented status codes means for a *job*, when it arrives as the
// `code` of a confirmation. `documented` is false for anything the table does not list,
// which is reported as such rather than being guessed into a bucket.
struct CloudPrntDisposition {
  JobOutcome outcome = JobOutcome::Unknown;
  FailureReason reason = FailureReason::Unknown;
  const char* meaning = "";
  bool documented = false;
};

// docs/wire-protocols.md §2 status table → docs/compatibility-brief.md §24 honesty.
// 200 is the only code that produces Done.
CloudPrntDisposition cloudPrntDisposition(int code) noexcept;

// The same table read as a *device* condition. False when pd::DeviceEvent — a closed
// enum mirrored into four wrappers — has no member for the code; those are recorded
// verbatim instead of being bent onto a neighbouring event.
bool cloudPrntDeviceEvent(int code, DeviceEvent* out) noexcept;

// The doc's own words for a code, or "" for one it does not list.
const char* cloudPrntCodeMeaning(int code) noexcept;

// "200 OK", the documented percent-encoded "200%20OK", and a bare "200" all parse to
// 200. False when there is no leading integer at all.
bool cloudPrntStatusCode(const std::string& text, int* out) noexcept;

// A job as seen from outside the lock.
struct CloudPrntJobView {
  std::string token;
  std::string printer_id;
  std::string media_type;                 // the type the bytes were submitted as
  std::vector<std::string> media_types;   // every type these same bytes may be served as
  size_t bytes = 0;
  uint32_t deliveries = 0;                // completed GETs; >1 after a re-download
  bool delivered = false;
  bool confirmed = false;
  bool terminal = false;
  int code = 0;                           // the confirmation code, 0 until one arrives
  std::string detail;                     // the doc's words for that code
  JobResult result;
  JobEvidence evidence;
};

// The repository plus the three printer-facing endpoints. Thread-safe: the HTTP server
// serves on a worker pool, so a poll, a download and a confirmation for one printer can
// genuinely be in flight at once.
class CloudPrntServer {
 public:
  CloudPrntServer() = default;
  CloudPrntServer(const CloudPrntServer&) = delete;
  CloudPrntServer& operator=(const CloudPrntServer&) = delete;

  bool addPrinter(const CloudPrntSpec& spec, std::string* error);
  bool known(const std::string& printer_id) const;
  size_t printerCount() const;
  std::vector<std::string> printerIds() const;

  // --- Application side ---------------------------------------------------------------

  // Hands bytes to a printer and returns the token they will be fetched under. The bytes
  // are already an encoded document — this path renders nothing, because the printer
  // pulls whatever it is given and no profile of ours is in the loop.
  bool submit(const std::string& printer_id, const std::vector<std::string>& media_types,
              std::string bytes, std::string* token, std::string* error);
  bool job(const std::string& printer_id, const std::string& token,
           CloudPrntJobView* out) const;
  std::vector<CloudPrntJobView> jobs(const std::string& printer_id) const;

  // --- Printer side (docs/wire-protocols.md §2) ----------------------------------------

  HttpResponse poll(const std::string& printer_id, const HttpRequest& request);
  HttpResponse fetchJob(const std::string& printer_id, const HttpRequest& request);
  HttpResponse confirm(const std::string& printer_id, const HttpRequest& request);

  // --- HTTP shapes for the application-facing routes -----------------------------------

  HttpResponse postJob(const std::string& printer_id, const HttpRequest& request);
  HttpResponse getJob(const std::string& printer_id, const std::string& token) const;
  dsl::Json jobsJson(const std::string& printer_id) const;
  dsl::Json listJson() const;

  // The last poll read as a device snapshot, in the same shape the pushed path reports.
  // Every flag stays unset until a poll actually asserted it: a code that says "cleaning"
  // says nothing about paper, and reporting it as healthy would be an invention.
  DeviceStatus deviceStatus(const std::string& printer_id) const;
  // Everything but `status`, which the agent adds so both printer kinds serialise their
  // device snapshot through one function.
  dsl::Json printerJson(const std::string& printer_id) const;

  size_t awaitingConfirmation() const;

  // The agent's existing device-event path: same callback shape as
  // PrinterDriver::subscribeDevices, so a subscriber cannot tell whether the event came
  // from an ASB frame or from a CloudPRNT poll. Callbacks run outside this class's lock —
  // a subscriber must never be able to deadlock a poll.
  void subscribeDevices(DriverDeviceEventCallback callback);

  static dsl::Json jobJson(const CloudPrntJobView& view);

 private:
  struct Job {
    std::string token;
    std::vector<std::string> media_types;  // front() is the type it was submitted as
    std::string bytes;                     // dropped once the job is confirmed
    size_t byte_count = 0;
    uint32_t deliveries = 0;
    bool confirmed = false;
    int code = 0;
    std::string detail;
    JobResult result;
    JobEvidence evidence;
  };

  struct Printer {
    CloudPrntSpec spec;
    std::string mac;            // normalised, bound identity (rule 2)
    bool mac_pinned = false;    // came from the config rather than from a first poll
    uint64_t identity_refusals = 0;

    bool polled = false;
    uint64_t polls = 0;
    MonotonicTime last_poll{};
    std::string unique_id;
    std::string asb;            // the poll's `status`, kept verbatim: it is ASB hex
    std::string status_text;    // the poll's `statusCode`, decoded ("200 OK")
    int status_code = 0;
    bool printing_in_progress = false;
    // The token the printer says it is holding. Informational: the queue is the server's
    // record, and a printer that has forgotten a token still owes the confirmation.
    std::string held_token;

    std::optional<DeviceEvent> fault;   // the last unresolved fault event
    bool announced = false;             // Online has been published once
    std::vector<std::string> events;    // bounded log, most recent last
    std::vector<std::string> conditions;  // codes pd::DeviceEvent has no member for

    std::deque<std::string> pending;   // tokens, FIFO; the head is the offered job
    std::deque<std::string> history;   // confirmed tokens, oldest first, for eviction
    std::unordered_map<std::string, Job> jobs;
  };

  Printer* find(const std::string& printer_id);
  const Printer* find(const std::string& printer_id) const;
  // Rule 2. Returns false — and counts a refusal — when `mac` names another printer.
  // An empty `mac` is accepted: the token is the second key, and a firmware that omits
  // the documented-but-optional parameter must not be locked out of its own queue.
  bool identityMatches(Printer& printer, const std::string& mac, bool adopt);
  std::string nextToken();
  static CloudPrntJobView view(const Printer& printer, const Job& job);
  // Under the lock: appends to the printer's logs and returns what has to be published
  // once the lock is gone.
  std::vector<DeviceEvent> observe(Printer& printer, int code);
  void publish(const std::string& printer_id, const std::vector<DeviceEvent>& events);

  mutable std::mutex mutex_;
  std::vector<Printer> printers_;
  std::unordered_map<std::string, size_t> by_id_;
  uint64_t sequence_ = 0;

  mutable std::mutex subscriber_mutex_;
  std::vector<DriverDeviceEventCallback> subscribers_;
};

}  // namespace pd::agent
