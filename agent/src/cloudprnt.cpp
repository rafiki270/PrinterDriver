#include "printerdriver/agent/cloudprnt.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>

namespace pd::agent {
namespace {

using dsl::Json;

Json errorJson(const std::string& message, const std::string& detail) {
  Json out = Json::object({});
  out.set("error", Json::string(message));
  if (!detail.empty()) {
    out.set("detail", Json::string(detail));
  }
  return out;
}

HttpResponse fail(int status, const std::string& message,
                  const std::string& detail = {}) {
  return HttpResponse::json(status, dsl::toJson(errorJson(message, detail)));
}

HttpResponse ok(int status, const Json& body) {
  return HttpResponse::json(status, dsl::toJson(body));
}

// The printer is told nothing about queues that are not its own. A token that belongs to
// another printer and a token that was never issued get the identical answer, which is
// both what §2 documents for the download (404) and the only answer that does not leak
// one lane's job into another lane's diagnostics.
HttpResponse unknownJob() { return fail(404, "unknown job"); }

// --- The documented status table (docs/wire-protocols.md §2) --------------------------
//
// One table, read three ways: as the doc's own words, as what a *confirmation* carrying
// the code means for the job, and as what the code says about the *device*. Keeping it
// in one place is what stops "410" from meaning paper-out in one function and a generic
// failure in the next.
//
// `event_valid` is false wherever pd::DeviceEvent — a closed enum mirrored into four
// wrappers (core/include/printerdriver/types.hpp) — has no member for the condition.
// Those codes are recorded verbatim instead of being bent onto a neighbouring event: an
// invented CoverOpen for "cleaning" would be a lie that survives into an operator's
// dashboard.
struct CodeRow {
  int code;
  const char* meaning;
  JobOutcome outcome;
  FailureReason reason;
  bool event_valid;
  DeviceEvent event;
};

constexpr CodeRow kCodes[] = {
    // 200 is the only code in the table that may produce Done
    // (docs/compatibility-brief.md §24: grade A is an explicit device completion).
    {200, "OK/success", JobOutcome::Done, FailureReason::None, false, DeviceEvent::Online},
    // Device condition, not job disposition. As a confirmation code none of these says
    // the receipt exists, so none of them may resolve to Done.
    {201, "output taken", JobOutcome::Unknown, FailureReason::Unknown, false,
     DeviceEvent::Online},
    {211, "paper low", JobOutcome::Unknown, FailureReason::Unknown, true,
     DeviceEvent::PaperNearEnd},
    {220, "printing", JobOutcome::Unknown, FailureReason::Unknown, false,
     DeviceEvent::Online},
    {221, "output present", JobOutcome::Unknown, FailureReason::Unknown, false,
     DeviceEvent::Online},
    {230, "cleaning", JobOutcome::Unknown, FailureReason::Unknown, false,
     DeviceEvent::Online},
    {231, "maintenance", JobOutcome::Unknown, FailureReason::Unknown, false,
     DeviceEvent::Online},
    // The four device faults. FailureReason's "Preflight" prefix names the condition the
    // engine refuses on, not the moment it was observed; the enum is closed and owned by
    // core, so these are the honest members for paper, cover and mechanism.
    {410, "paper out", JobOutcome::Failed, FailureReason::PreflightPaperOut, true,
     DeviceEvent::PaperOut},
    {411, "jam", JobOutcome::Failed, FailureReason::PreflightHardwareError, true,
     DeviceEvent::RecoverableError},
    {412, "roll position", JobOutcome::Failed, FailureReason::PreflightHardwareError, true,
     DeviceEvent::RecoverableError},
    {420, "cover open", JobOutcome::Failed, FailureReason::PreflightCoverOpen, true,
     DeviceEvent::CoverOpen},
    // Job-level refusals: the printer had the bytes and could not render them. They say
    // nothing about the device's condition, so they publish no DeviceEvent.
    {510, "incompatible media", JobOutcome::Failed, FailureReason::Unsupported, false,
     DeviceEvent::Online},
    {511, "decode failure", JobOutcome::Failed, FailureReason::Unsupported, false,
     DeviceEvent::Online},
    {512, "unsupported media version", JobOutcome::Failed, FailureReason::Unsupported,
     false, DeviceEvent::Online},
    {520, "job timeout", JobOutcome::Failed, FailureReason::TimeoutAwaitingCompletion,
     false, DeviceEvent::Online},
    // "too large" is a capacity refusal by the device. FailureReason::QueueOverflow is
    // reserved for the print-queue addon (docs/sdk-spec.md §12) and is not borrowed here;
    // Unsupported carries the fact that this device cannot take this job.
    {521, "job too large", JobOutcome::Failed, FailureReason::Unsupported, false,
     DeviceEvent::Online},
    // The printer rejected the server-settings document *we* serve. It is our
    // configuration that is wrong, not the paper — recorded with the doc's words so the
    // enum's coarseness does not erase which of the two it was.
    {1000, "server settings JSON error", JobOutcome::Failed, FailureReason::Unsupported,
     false, DeviceEvent::Online},
    {1001, "server settings JSON error", JobOutcome::Failed, FailureReason::Unsupported,
     false, DeviceEvent::Online},
};

const CodeRow* findCode(int code) noexcept {
  for (const CodeRow& row : kCodes) {
    if (row.code == code) {
      return &row;
    }
  }
  return nullptr;
}

// The codes that assert nothing is wrong with paper, cover or mechanism. Used for
// recovery transitions and for the device snapshot; deliberately not "everything 2xx",
// because 230 cleaning and 231 maintenance assert no such thing.
bool healthy(int code) noexcept {
  return code == 200 || code == 201 || code == 220 || code == 221;
}

std::string lowercased(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// "00:11:62:AA:BB:CC", "00-11-62-aa-bb-cc" and "001162aabbcc" are one identity. Firmware,
// configuration files and QR-code labels all spell a MAC differently, and rule 2 has to
// compare the device, not the punctuation.
std::string normaliseMac(const std::string& text) {
  std::string out;
  for (const unsigned char c : text) {
    if (std::isalnum(c) != 0) {
      out.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return out;
}

// Media types compare case-insensitively and without their parameters: a printer that
// asks for `text/plain; charset=utf-8` is asking for the `text/plain` we advertised.
std::string normaliseMedia(const std::string& text) {
  std::string out = lowercased(text);
  const size_t semicolon = out.find(';');
  if (semicolon != std::string::npos) {
    out.erase(semicolon);
  }
  size_t begin = 0;
  while (begin < out.size() && std::isspace(static_cast<unsigned char>(out[begin])) != 0) {
    ++begin;
  }
  size_t end = out.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(out[end - 1])) != 0) {
    --end;
  }
  return out.substr(begin, end - begin);
}

bool decodeBase64(const std::string& text, std::string* out) {
  const auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  uint32_t accumulator = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    const int digit = value(c);
    if (digit < 0) {
      return false;
    }
    accumulator = (accumulator << 6) | static_cast<uint32_t>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<char>((accumulator >> bits) & 0xFFu));
    }
  }
  return true;
}

const std::string& stringField(const Json& json, const char* key,
                               const std::string& fallback) {
  const Json* value = json.find(key);
  return value != nullptr && value->isString() ? value->asString() : fallback;
}

Json mediaTypesJson(const std::vector<std::string>& types) {
  Json list = Json::array({});
  for (const std::string& type : types) {
    list.push(Json::string(type));
  }
  return list;
}

void appendBounded(std::vector<std::string>* log, std::string entry, size_t limit) {
  log->push_back(std::move(entry));
  if (log->size() > limit) {
    log->erase(log->begin(), log->begin() + static_cast<std::ptrdiff_t>(log->size() - limit));
  }
}

constexpr size_t kEventLog = 16;
constexpr size_t kConditionLog = 8;

}  // namespace

// --- The table, read from outside -------------------------------------------------------

CloudPrntDisposition cloudPrntDisposition(int code) noexcept {
  CloudPrntDisposition out;
  if (const CodeRow* row = findCode(code); row != nullptr) {
    out.outcome = row->outcome;
    out.reason = row->reason;
    out.meaning = row->meaning;
    out.documented = true;
    return out;
  }
  // Not in §2's table. The HTTP-shaped ranges still carry meaning — the printer chose a
  // 4xx/5xx to say something went wrong — but the *specific* condition is unknown, and
  // saying so is the point of the tri-state.
  if (code >= 400 && code <= 599) {
    out.outcome = JobOutcome::Failed;
    out.reason = FailureReason::Unknown;
    out.meaning = "undocumented failure code";
    return out;
  }
  out.outcome = JobOutcome::Unknown;
  out.reason = FailureReason::Unknown;
  out.meaning = "undocumented status code";
  return out;
}

bool cloudPrntDeviceEvent(int code, DeviceEvent* out) noexcept {
  const CodeRow* row = findCode(code);
  if (row == nullptr || !row->event_valid) {
    return false;
  }
  if (out != nullptr) {
    *out = row->event;
  }
  return true;
}

const char* cloudPrntCodeMeaning(int code) noexcept {
  const CodeRow* row = findCode(code);
  return row != nullptr ? row->meaning : "";
}

bool cloudPrntStatusCode(const std::string& text, int* out) noexcept {
  size_t pos = 0;
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  if (pos >= text.size() || std::isdigit(static_cast<unsigned char>(text[pos])) == 0) {
    return false;
  }
  long value = 0;
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    value = value * 10 + (text[pos] - '0');
    if (value > 100000) {  // a status code, not a payload
      return false;
    }
    ++pos;
  }
  if (out != nullptr) {
    *out = static_cast<int>(value);
  }
  return true;
}

// --- Registry ---------------------------------------------------------------------------

bool CloudPrntServer::addPrinter(const CloudPrntSpec& spec, std::string* error) {
  auto refuse = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (spec.id.empty()) {
    return refuse("a cloudprnt printer needs an id");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (by_id_.find(spec.id) != by_id_.end()) {
    return refuse("cloudprnt printer id already owned: " + spec.id);
  }
  Printer printer;
  printer.spec = spec;
  if (printer.spec.media_types.empty()) {
    printer.spec.media_types.emplace_back(kCloudPrntDefaultMediaType);
  }
  if (printer.spec.max_pending == 0) {
    printer.spec.max_pending = 1;
  }
  printer.mac = normaliseMac(spec.mac);
  printer.mac_pinned = !printer.mac.empty();
  by_id_[spec.id] = printers_.size();
  printers_.push_back(std::move(printer));
  return true;
}

CloudPrntServer::Printer* CloudPrntServer::find(const std::string& printer_id) {
  const auto it = by_id_.find(printer_id);
  return it == by_id_.end() ? nullptr : &printers_[it->second];
}

const CloudPrntServer::Printer* CloudPrntServer::find(const std::string& printer_id) const {
  const auto it = by_id_.find(printer_id);
  return it == by_id_.end() ? nullptr : &printers_[it->second];
}

bool CloudPrntServer::known(const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return by_id_.find(printer_id) != by_id_.end();
}

size_t CloudPrntServer::printerCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return printers_.size();
}

std::vector<std::string> CloudPrntServer::printerIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(printers_.size());
  for (const Printer& printer : printers_) {
    out.push_back(printer.spec.id);
  }
  return out;
}

// docs/wire-protocols.md §2 rule 2, "key by printer identity + token". The token is the
// second key and is random, but identity is the one that stops a second device on the
// segment from draining a queue it can see: a printer polling a route bound to another
// MAC is answered as though the route did not exist.
bool CloudPrntServer::identityMatches(Printer& printer, const std::string& mac,
                                      bool adopt) {
  if (mac.empty()) {
    // Not every firmware sends the parameter on every request, and §2 lists it without
    // making it the credential. A pinned route still refuses an *unverifiable* poll,
    // because adopting nothing would leave the pin unenforced.
    if (printer.mac_pinned && adopt) {
      ++printer.identity_refusals;
      return false;
    }
    return true;
  }
  if (printer.mac.empty()) {
    if (!adopt) {
      // Nothing has claimed the route yet, so there is no identity to contradict. The
      // download still needs a token this printer's queue actually holds.
      return true;
    }
    // Trust-on-first-use. Pinning `mac` in the config closes the window; leaving it open
    // is what makes a printer that was swapped for a spare work without an edit.
    printer.mac = mac;
    return true;
  }
  if (printer.mac != mac) {
    ++printer.identity_refusals;
    return false;
  }
  return true;
}

std::string CloudPrntServer::nextToken() {
  // Called under mutex_. Random rather than sequential: the token travels in a query
  // string on an unauthenticated LAN, and guessing it is the only other way to reach a
  // job that identity checking would otherwise refuse.
  static std::mt19937_64 engine(std::random_device{}());
  static const char* digits = "0123456789abcdef";
  const uint64_t bits = engine();
  std::string out = "cp-";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(digits[(bits >> shift) & 0xFu]);
  }
  out.push_back('-');
  out += std::to_string(++sequence_);
  return out;
}

void CloudPrntServer::subscribeDevices(DriverDeviceEventCallback callback) {
  if (!callback) {
    return;
  }
  std::lock_guard<std::mutex> lock(subscriber_mutex_);
  subscribers_.push_back(std::move(callback));
}

void CloudPrntServer::publish(const std::string& printer_id,
                              const std::vector<DeviceEvent>& events) {
  if (events.empty()) {
    return;
  }
  // Copied out and invoked with no lock held: a subscriber that asks the agent something
  // must not be able to deadlock against a poll that is holding this class's lock.
  std::vector<DriverDeviceEventCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    callbacks = subscribers_;
  }
  for (const DriverDeviceEventCallback& callback : callbacks) {
    for (const DeviceEvent event : events) {
      callback(printer_id, event);
    }
  }
}

std::vector<DeviceEvent> CloudPrntServer::observe(Printer& printer, int code) {
  std::vector<DeviceEvent> events;
  if (!printer.announced) {
    printer.announced = true;
    // A poll arriving is the only liveness evidence this topology produces: we cannot
    // dial the printer, so "it spoke to us" is the whole of Online.
    events.push_back(DeviceEvent::Online);
  }
  DeviceEvent mapped = DeviceEvent::Online;
  if (cloudPrntDeviceEvent(code, &mapped)) {
    if (!printer.fault.has_value() || *printer.fault != mapped) {
      printer.fault = mapped;
      events.push_back(mapped);
    }
  } else if (printer.fault.has_value() && healthy(code)) {
    // The recovery edge. The closed enum has a member for coming back from each fault
    // §2's table can produce, so the transition is reported rather than forgotten.
    switch (*printer.fault) {
      case DeviceEvent::PaperOut:
      case DeviceEvent::PaperNearEnd:
        events.push_back(DeviceEvent::PaperOk);
        break;
      case DeviceEvent::CoverOpen:
        events.push_back(DeviceEvent::CoverClosed);
        break;
      default:
        events.push_back(DeviceEvent::Online);
        break;
    }
    printer.fault.reset();
  }
  for (const DeviceEvent event : events) {
    appendBounded(&printer.events, to_string(event), kEventLog);
  }
  // Everything §2 lists that pd::DeviceEvent has no member for — 201 output taken, 220
  // printing, 230 cleaning, 231 maintenance, the 5xx job refusals — is recorded verbatim
  // instead of being mapped onto a neighbouring event. Recorded on change only: a print
  // run would otherwise fill the log with "220 printing".
  if (code != 0 && code != 200 && code != printer.status_code &&
      !cloudPrntDeviceEvent(code, nullptr)) {
    const char* meaning = cloudPrntCodeMeaning(code);
    appendBounded(&printer.conditions,
                  std::to_string(code) + " " +
                      (meaning[0] != '\0' ? meaning : "not in the documented table"),
                  kConditionLog);
  }
  return events;
}

// --- Application side --------------------------------------------------------------------

bool CloudPrntServer::submit(const std::string& printer_id,
                             const std::vector<std::string>& media_types,
                             std::string bytes, std::string* token, std::string* error) {
  auto refuse = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  std::lock_guard<std::mutex> lock(mutex_);
  Printer* printer = find(printer_id);
  if (printer == nullptr) {
    return refuse("unknown cloudprnt printer: " + printer_id);
  }
  if (bytes.empty()) {
    return refuse("a cloudprnt job needs bytes");
  }
  if (printer->pending.size() >= printer->spec.max_pending) {
    return refuse("cloudprnt queue is full for " + printer_id +
                  "; the printer is not collecting its jobs");
  }
  std::vector<std::string> types;
  for (const std::string& requested : media_types) {
    const std::string normalised = normaliseMedia(requested);
    if (normalised.empty()) {
      continue;
    }
    // The advertised list is a contract the poll answer publishes. Queueing a type the
    // printer was never told about produces a device that downloads and 415s forever, so
    // it is refused here, where the caller can still see why.
    const auto known = std::find_if(printer->spec.media_types.begin(),
                                    printer->spec.media_types.end(),
                                    [&](const std::string& advertised) {
                                      return normaliseMedia(advertised) == normalised;
                                    });
    if (known == printer->spec.media_types.end()) {
      return refuse("media type is not advertised by " + printer_id + ": " + requested);
    }
    types.push_back(*known);
  }
  if (types.empty()) {
    types.push_back(printer->spec.media_types.front());
  }

  Job job;
  job.token = nextToken();
  job.media_types = std::move(types);
  job.byte_count = bytes.size();
  job.bytes = std::move(bytes);
  // Non-terminal from the first instant and honest about why: nothing has left this host,
  // so there is no evidence of any kind yet (docs/compatibility-brief.md §24, grade E is
  // already a *successful write* — this is below it).
  job.result = JobResult::unknown(ConfidenceLevel::TransportAccepted);
  job.evidence = JobEvidence{ConfidenceGrade::E_TransportOnly,
                             CompletionAuthority::TransportOnly, "none"};
  job.result.with(job.evidence);
  if (token != nullptr) {
    *token = job.token;
  }
  printer->pending.push_back(job.token);
  printer->jobs.emplace(job.token, std::move(job));
  return true;
}

CloudPrntJobView CloudPrntServer::view(const Printer& printer, const Job& job) {
  CloudPrntJobView out;
  out.token = job.token;
  out.printer_id = printer.spec.id;
  out.media_type = job.media_types.front();
  out.media_types = job.media_types;
  out.bytes = job.byte_count;
  out.deliveries = job.deliveries;
  out.delivered = job.deliveries > 0;
  out.confirmed = job.confirmed;
  // Terminal means confirmed, and nothing else. A job that was downloaded but never
  // confirmed is still open however long it has been: a download is not a receipt.
  out.terminal = job.confirmed;
  out.code = job.code;
  out.detail = job.detail;
  out.result = job.result;
  out.evidence = job.evidence;
  return out;
}

bool CloudPrntServer::job(const std::string& printer_id, const std::string& token,
                          CloudPrntJobView* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Printer* printer = find(printer_id);
  if (printer == nullptr) {
    return false;
  }
  const auto it = printer->jobs.find(token);
  if (it == printer->jobs.end()) {
    return false;
  }
  if (out != nullptr) {
    *out = view(*printer, it->second);
  }
  return true;
}

std::vector<CloudPrntJobView> CloudPrntServer::jobs(const std::string& printer_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<CloudPrntJobView> out;
  const Printer* printer = find(printer_id);
  if (printer == nullptr) {
    return out;
  }
  for (const std::string& token : printer->pending) {
    const auto it = printer->jobs.find(token);
    if (it != printer->jobs.end()) {
      out.push_back(view(*printer, it->second));
    }
  }
  for (const std::string& token : printer->history) {
    const auto it = printer->jobs.find(token);
    if (it != printer->jobs.end()) {
      out.push_back(view(*printer, it->second));
    }
  }
  return out;
}

// --- Printer side ------------------------------------------------------------------------

HttpResponse CloudPrntServer::poll(const std::string& printer_id,
                                   const HttpRequest& request) {
  Json body;
  std::string parse_error;
  if (!dsl::tryParseJson(request.body, &body, &parse_error) || !body.isObject()) {
    // The poll document is specified (docs/wire-protocols.md §2). A body that is not it
    // is refused rather than guessed at: guessing would mean handing a job to a device we
    // cannot even name.
    return fail(400, "malformed CloudPRNT poll", parse_error);
  }

  static const std::string kEmpty;
  const std::string mac = normaliseMac(stringField(body, "printerMAC", kEmpty));
  const std::string unique_id = stringField(body, "uniqueID", kEmpty);
  const std::string status_text = percentDecode(stringField(body, "statusCode", kEmpty));
  const std::string asb = stringField(body, "status", kEmpty);
  const std::string held = stringField(body, "jobToken", kEmpty);
  const Json* printing = body.find("printingInProgress");

  int code = 0;
  const bool has_code = cloudPrntStatusCode(status_text, &code);

  std::vector<DeviceEvent> events;
  HttpResponse response;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Printer* printer = find(printer_id);
    if (printer == nullptr) {
      return unknownJob();
    }
    if (!identityMatches(*printer, mac, true)) {
      // Two printers on one route is this protocol's version of the single-owner
      // violation the pushed path calls foreignWriterDetected: whichever device polls
      // first would otherwise take receipts addressed to the other one.
      return fail(404, "not found", "cloudprnt route is bound to another printer");
    }

    events = observe(*printer, has_code ? code : 0);
    printer->polled = true;
    ++printer->polls;
    printer->last_poll = MonotonicClock::now();
    if (!unique_id.empty()) {
      printer->unique_id = unique_id;
    }
    printer->asb = asb;
    printer->status_text = status_text;
    if (has_code) {
      printer->status_code = code;
    }
    printer->held_token = held;
    printer->printing_in_progress = printing != nullptr && printing->truthy();

    // Strictly one job at a time, head first, and the *same* job for as long as it is
    // unconfirmed (rule 1 + rule 3). Offering the next one while the printer still owes a
    // confirmation would be the server deciding the previous receipt exists.
    const Job* offered = nullptr;
    while (!printer->pending.empty() && offered == nullptr) {
      const auto it = printer->jobs.find(printer->pending.front());
      if (it == printer->jobs.end()) {
        printer->pending.pop_front();
        continue;
      }
      offered = &it->second;
    }

    Json out = Json::object({});
    out.set("jobReady", Json::boolean(offered != nullptr));
    // With a job waiting the list is what *that job* can be served as, so `type=` on the
    // download is a choice the printer makes from types we can actually satisfy. Idle, it
    // is the printer's advertised list.
    out.set("mediaTypes", mediaTypesJson(offered != nullptr ? offered->media_types
                                                            : printer->spec.media_types));
    // Always present, empty when idle: a stable shape is easier for a firmware to parse
    // than an optional member.
    out.set("jobToken", Json::string(offered != nullptr ? offered->token : std::string()));
    out.set("deleteMethod", Json::string("DELETE"));
    // jobGetUrl / jobConfirmationUrl are deliberately omitted. They are optional in §2,
    // and absent them the printer reuses the URL it polled — which is this route. We
    // cannot honestly synthesise an absolute URL: the only host we know is the one in a
    // Host header that a reverse proxy may have rewritten, and a wrong absolute URL sends
    // the download somewhere the job is not.
    response = ok(200, out);
  }
  publish(printer_id, events);
  return response;
}

HttpResponse CloudPrntServer::fetchJob(const std::string& printer_id,
                                       const HttpRequest& request) {
  const std::string token = request.queryValue("token");
  const std::string mac = normaliseMac(request.queryValue("mac"));
  const std::string type = normaliseMedia(request.queryValue("type"));

  std::lock_guard<std::mutex> lock(mutex_);
  Printer* printer = find(printer_id);
  if (printer == nullptr) {
    return unknownJob();
  }
  if (!identityMatches(*printer, mac, false)) {
    return unknownJob();
  }

  std::string wanted = token;
  if (wanted.empty()) {
    // Some firmware omits the parameter and simply downloads whatever the poll offered.
    // The head of the queue *is* what the poll offered, so this stays deterministic.
    if (printer->pending.empty()) {
      return unknownJob();
    }
    wanted = printer->pending.front();
  }
  const auto it = printer->jobs.find(wanted);
  if (it == printer->jobs.end() || it->second.confirmed) {
    // Confirmed means retired: the DELETE is what deletes, and after it there is nothing
    // left to download.
    return unknownJob();
  }
  Job& job = it->second;

  std::string negotiated = job.media_types.front();
  if (!type.empty() && type != "*/*") {
    const auto match = std::find_if(job.media_types.begin(), job.media_types.end(),
                                    [&](const std::string& candidate) {
                                      return normaliseMedia(candidate) == type;
                                    });
    if (match == job.media_types.end()) {
      // 415 is documented for exactly this (§2: "results 200/401 optional Basic/404/415").
      // The job survives: a printer asking for the wrong type has not consumed anything.
      return fail(415, "unsupported media type", request.queryValue("type"));
    }
    negotiated = *match;
  }

  // Rule 3, the one that makes an interrupted transfer recoverable: the download is
  // counted, never consumed. The bytes stay until a DELETE says the paper exists.
  ++job.deliveries;
  HttpResponse out;
  out.status = 200;
  out.content_type = negotiated;
  out.body = job.bytes;
  return out;
}

HttpResponse CloudPrntServer::confirm(const std::string& printer_id,
                                      const HttpRequest& request) {
  const std::string token = request.queryValue("token");
  const std::string mac = normaliseMac(request.queryValue("mac"));
  bool has_code_param = false;
  const std::string code_text = request.queryValue("code", &has_code_param);
  bool retried = false;
  const std::string retry = request.queryValue("retry", &retried);

  int code = 0;
  const bool parsed = has_code_param && cloudPrntStatusCode(code_text, &code);

  std::lock_guard<std::mutex> lock(mutex_);
  Printer* printer = find(printer_id);
  if (printer == nullptr) {
    return unknownJob();
  }
  if (!identityMatches(*printer, mac, false)) {
    return unknownJob();
  }
  std::string wanted = token;
  if (wanted.empty()) {
    if (printer->pending.empty()) {
      return unknownJob();
    }
    wanted = printer->pending.front();
  }
  const auto it = printer->jobs.find(wanted);
  if (it == printer->jobs.end()) {
    return unknownJob();
  }
  Job& job = it->second;

  Json out = Json::object({});
  out.set("token", Json::string(job.token));
  if (retried) {
    out.set("retry", Json::string(retry));
  }
  if (job.confirmed) {
    // §2 documents the confirmation as retried (`retry=x` may appear on it). A printer
    // repeating a DELETE it already delivered is behaving correctly, so it gets the same
    // answer again — 404 here would make a well-behaved firmware retry forever, and the
    // first code is the one that was evidence.
    out.set("ok", Json::boolean(true));
    out.set("idempotent", Json::boolean(true));
    out.set("code", Json::number(static_cast<double>(job.code)));
    out.set("outcome", Json::string(to_string(job.result.outcome)));
    return ok(200, out);
  }

  const CloudPrntDisposition disposition =
      parsed ? cloudPrntDisposition(code)
             : CloudPrntDisposition{JobOutcome::Unknown, FailureReason::Unknown,
                                    "confirmation carried no status code", false};

  job.confirmed = true;
  job.code = parsed ? code : 0;
  job.detail = disposition.meaning;
  if (disposition.outcome == JobOutcome::Done) {
    // docs/compatibility-brief.md §24 grade A: a job-level statement by the mechanism
    // that moved the paper. PrintConfirmed and no further — the confirmation says the job
    // was printed, never that the blade fired cleanly, so the cut rungs stay unclimbed.
    job.evidence = JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                               CompletionAuthority::PhysicalPrinter, kCloudPrntMethod};
    job.result = JobResult::done(ConfidenceLevel::PrintConfirmed);
  } else if (disposition.outcome == JobOutcome::Failed) {
    // Grade A on a failure too: the *claim* is Failed, and the evidence behind it is
    // still a job-level statement from the printer itself. Grading it lower would hide
    // how well attested the failure is.
    job.evidence = JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                               CompletionAuthority::PhysicalPrinter, kCloudPrntMethod};
    job.result = JobResult::failed(disposition.reason, ConfidenceLevel::TransportAccepted);
  } else if (parsed) {
    // A documented code that describes the device rather than the job (201, 220, 230 …),
    // or one the table does not list. The printer is finished with the job and said
    // nothing about the paper, so the outcome is Unknown and stays there — the agent
    // never retries out of Unknown, and it never resolves it either.
    job.evidence = JobEvidence{ConfidenceGrade::A_JobLevelConfirmation,
                               CompletionAuthority::PhysicalPrinter, kCloudPrntMethod};
    job.result = JobResult::unknown(ConfidenceLevel::TransportAccepted);
  } else {
    // A DELETE with no readable code is a delete, not evidence: it retires the job
    // (§2: only the DELETE deletes) and claims nothing about it.
    job.evidence = JobEvidence{ConfidenceGrade::E_TransportOnly,
                               CompletionAuthority::TransportOnly, kCloudPrntMethod};
    job.result = JobResult::unknown(ConfidenceLevel::TransportAccepted);
  }
  job.result.with(job.evidence);
  // Retention ends exactly here, at rule 1's boundary. The result outlives the bytes so a
  // retried DELETE and an application asking how the receipt went both still get answers.
  job.bytes.clear();
  job.bytes.shrink_to_fit();

  const auto queued = std::find(printer->pending.begin(), printer->pending.end(), job.token);
  if (queued != printer->pending.end()) {
    printer->pending.erase(queued);
  }
  printer->history.push_back(job.token);
  while (printer->history.size() > printer->spec.max_history) {
    const std::string evicted = printer->history.front();
    printer->history.pop_front();
    printer->jobs.erase(evicted);
  }

  out.set("ok", Json::boolean(true));
  out.set("idempotent", Json::boolean(false));
  out.set("code", Json::number(static_cast<double>(job.code)));
  out.set("outcome", Json::string(to_string(job.result.outcome)));
  out.set("detail", Json::string(job.detail));
  return ok(200, out);
}

// --- Application-facing HTTP ---------------------------------------------------------------

HttpResponse CloudPrntServer::postJob(const std::string& printer_id,
                                      const HttpRequest& request) {
  std::vector<std::string> media_types;
  std::string bytes;

  const std::string* content_type = request.header("content-type");
  const std::string declared =
      content_type != nullptr ? normaliseMedia(*content_type) : std::string();
  if (declared.empty() || declared == "application/json") {
    Json body;
    std::string parse_error;
    if (!dsl::tryParseJson(request.body, &body, &parse_error) || !body.isObject()) {
      return fail(400, "malformed JSON", parse_error);
    }
    if (const Json* value = body.find("mediaType");
        value != nullptr && value->isString()) {
      media_types.push_back(value->asString());
    }
    if (const Json* value = body.find("mediaTypes"); value != nullptr && value->isArray()) {
      for (const Json& entry : value->asArray()) {
        if (entry.isString()) {
          media_types.push_back(entry.asString());
        }
      }
    }
    if (const Json* value = body.find("base64"); value != nullptr && value->isString()) {
      if (!decodeBase64(value->asString(), &bytes)) {
        return fail(400, "base64 is not valid base64");
      }
    } else if (const Json* text = body.find("text"); text != nullptr && text->isString()) {
      bytes = text->asString();
    } else {
      return fail(400, "a cloudprnt job needs text or base64");
    }
  } else {
    // A raw body: the application hands over an already-encoded document and names it
    // with the Content-Type. This is the natural shape here, because nothing in this
    // process renders for a CloudPRNT printer — the device pulls whatever it is given.
    media_types.push_back(*content_type);
    bytes = request.body;
  }

  std::string token;
  std::string error;
  if (!submit(printer_id, media_types, std::move(bytes), &token, &error)) {
    const bool full = error.find("queue is full") != std::string::npos;
    const bool unknown = error.find("unknown cloudprnt printer") != std::string::npos;
    return fail(full ? 503 : (unknown ? 404 : 400), "cloudprnt job rejected", error);
  }
  CloudPrntJobView created;
  if (!job(printer_id, token, &created)) {
    return fail(500, "the cloudprnt job vanished after being queued", token);
  }
  return ok(201, jobJson(created));
}

HttpResponse CloudPrntServer::getJob(const std::string& printer_id,
                                     const std::string& token) const {
  CloudPrntJobView found;
  if (!job(printer_id, token, &found)) {
    return fail(404, "unknown job", token);
  }
  return ok(200, jobJson(found));
}

Json CloudPrntServer::jobJson(const CloudPrntJobView& entry) {
  Json out = Json::object({});
  out.set("token", Json::string(entry.token));
  out.set("printerId", Json::string(entry.printer_id));
  out.set("mediaType", Json::string(entry.media_type));
  out.set("mediaTypes", mediaTypesJson(entry.media_types));
  out.set("bytes", Json::number(static_cast<double>(entry.bytes)));
  // The CloudPRNT lifecycle, deliberately not pd::JobState. That enum describes a job the
  // engine pushed — preflight, SendStarted, a cut command going out — and none of those
  // moments exist when the printer pulls. Borrowing its names would let a reader believe
  // an engine ran.
  out.set("state",
          Json::string(entry.confirmed ? "confirmed"
                                       : (entry.delivered ? "delivered" : "queued")));
  out.set("delivered", Json::boolean(entry.delivered));
  out.set("deliveries", Json::number(static_cast<double>(entry.deliveries)));
  out.set("terminal", Json::boolean(entry.terminal));
  // Only a confirmed job has an outcome. A downloaded-but-unconfirmed job is genuinely
  // open, and the absence of the field is the honest way to say so.
  if (entry.terminal) {
    out.set("outcome", Json::string(to_string(entry.result.outcome)));
    out.set("code", Json::number(static_cast<double>(entry.code)));
    if (!entry.detail.empty()) {
      out.set("detail", Json::string(entry.detail));
    }
    if (entry.result.reason != FailureReason::None) {
      out.set("reason", Json::string(to_string(entry.result.reason)));
    }
  }
  out.set("grade", Json::string(gradeLetter(entry.result.grade)));
  out.set("authority", Json::string(to_string(entry.result.authority)));
  out.set("method", Json::string(entry.result.method));
  out.set("confidence", Json::string(to_string(entry.result.confidence)));
  return out;
}

Json CloudPrntServer::jobsJson(const std::string& printer_id) const {
  Json list = Json::array({});
  for (const CloudPrntJobView& entry : jobs(printer_id)) {
    list.push(jobJson(entry));
  }
  Json out = Json::object({});
  out.set("jobs", std::move(list));
  return out;
}

// --- Reporting -------------------------------------------------------------------------

DeviceStatus CloudPrntServer::deviceStatus(const std::string& printer_id) const {
  DeviceStatus out;
  std::lock_guard<std::mutex> lock(mutex_);
  const Printer* printer = find(printer_id);
  if (printer == nullptr || !printer->polled) {
    return out;
  }
  // "Connected" for a device that dials us can only mean "it has spoken to this process".
  out.connected = true;
  out.online = true;
  out.observed = printer->status_code != 0;
  if (!out.observed) {
    return out;
  }
  const int code = printer->status_code;
  if (healthy(code)) {
    out.cover_open = false;
    out.paper_out = false;
    out.paper_near_end = false;
    out.recoverable_error = false;
    return out;
  }
  // Everything else asserts one condition and stays silent about the rest, so only that
  // one flag is set. A "cleaning" code is not evidence that the paper is fine, and
  // reporting it as such is the invention this whole codebase exists to avoid.
  switch (code) {
    case 211:
      out.paper_near_end = true;
      out.paper_out = false;
      break;
    case 410:
      out.paper_out = true;
      break;
    case 420:
      out.cover_open = true;
      break;
    case 411:
    case 412:
      out.recoverable_error = true;
      break;
    default:
      break;
  }
  return out;
}

Json CloudPrntServer::printerJson(const std::string& printer_id) const {
  Json out = Json::object({});
  std::lock_guard<std::mutex> lock(mutex_);
  const Printer* printer = find(printer_id);
  if (printer == nullptr) {
    return out;
  }
  out.set("id", Json::string(printer->spec.id));
  // A CloudPRNT printer is not a lane this agent can write to, and saying so in the same
  // field the pushed printers use for their endpoint keeps a reader from assuming it is.
  out.set("kind", Json::string("cloudprnt"));
  out.set("transport", Json::string("cloudprnt (printer polls)"));
  out.set("completion", Json::string(kCloudPrntMethod));
  out.set("grade", Json::string(gradeLetter(ConfidenceGrade::A_JobLevelConfirmation)));
  // The grade describes what a *confirmation* is worth, not what the printer is worth.
  // Until the DELETE lands, a job on this printer has no grade at all, and this field
  // must not be read as one.
  out.set("gradeAppliesWhen", Json::string("confirmed"));
  out.set("mediaTypes", mediaTypesJson(printer->spec.media_types));
  out.set("printerMAC", printer->mac.empty() ? Json::null() : Json::string(printer->mac));
  out.set("macPinned", Json::boolean(printer->mac_pinned));
  out.set("uniqueId",
          printer->unique_id.empty() ? Json::null() : Json::string(printer->unique_id));
  out.set("polls", Json::number(static_cast<double>(printer->polls)));
  if (printer->polled) {
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         MonotonicClock::now() - printer->last_poll)
                         .count();
    out.set("lastPollAgeMs", Json::number(static_cast<double>(age)));
  } else {
    // Never polled: the printer has never been heard from, which is not the same as a
    // healthy printer that is idle.
    out.set("lastPollAgeMs", Json::null());
  }
  out.set("statusCode", printer->status_code == 0
                            ? Json::null()
                            : Json::number(static_cast<double>(printer->status_code)));
  out.set("statusText", Json::string(printer->status_text));
  out.set("statusMeaning", Json::string(cloudPrntCodeMeaning(printer->status_code)));
  out.set("asb", Json::string(printer->asb));
  out.set("printingInProgress", Json::boolean(printer->printing_in_progress));
  out.set("heldToken", Json::string(printer->held_token));
  out.set("pending", Json::number(static_cast<double>(printer->pending.size())));
  out.set("confirmed", Json::number(static_cast<double>(printer->history.size())));
  // The CloudPRNT shape of the single-owner violation: a device claiming a route that is
  // bound to another MAC. Counted rather than swallowed, exactly like foreignWriterDetected
  // on the pushed path (docs/sdk-spec.md §14).
  out.set("identityRefusals",
          Json::number(static_cast<double>(printer->identity_refusals)));
  Json events = Json::array({});
  for (const std::string& entry : printer->events) {
    events.push(Json::string(entry));
  }
  out.set("events", std::move(events));
  Json conditions = Json::array({});
  for (const std::string& entry : printer->conditions) {
    conditions.push(Json::string(entry));
  }
  // Documented codes pd::DeviceEvent has no member for, kept verbatim.
  out.set("conditions", std::move(conditions));
  return out;
}

Json CloudPrntServer::listJson() const {
  std::vector<std::string> ids = printerIds();
  Json list = Json::array({});
  for (const std::string& id : ids) {
    list.push(printerJson(id));
  }
  Json out = Json::object({});
  out.set("cloudprnt", std::move(list));
  return out;
}

size_t CloudPrntServer::awaitingConfirmation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (const Printer& printer : printers_) {
    total += printer.pending.size();
  }
  return total;
}

}  // namespace pd::agent
