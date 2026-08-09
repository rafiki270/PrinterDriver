#include "printerdriver/agent/agent.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

#include "printerdriver/device_profiles.hpp"
#include "printerdriver/receipt_dsl.hpp"

namespace pd::agent {
namespace {

using dsl::Json;

Json errorJson(const std::string& message, const std::string& detail = {}) {
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

bool decodeBase64(std::string_view text, std::vector<uint8_t>* out) {
  static const auto value = [](char c) -> int {
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
      out->push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
    }
  }
  return true;
}

bool profileByName(const std::string& name, CapabilityProfile* out) {
  if (name.empty() || name == "generic") {
    *out = generic_escpos();
    return true;
  }
  if (name == "xp-s260m") {
    *out = xp_s260m();
    return true;
  }
  if (devices::exists(name)) {
    *out = devices::byName(name);
    return true;
  }
  return false;
}

// docs/api.md §4: FailedKnown means nothing reached the printer; Unknown means bytes
// did. That distinction is what the evidence document has to preserve, because it is
// the difference between "safe to reprint" and "ask an operator".
bool bytesLeftTheHost(JobState state) noexcept {
  switch (state) {
    case JobState::BytesSent:
    case JobState::PrintConfirmed:
    case JobState::CutCommandProcessed:
    case JobState::DoneSoftware:
    case JobState::PhysicallyVerified:
    case JobState::Unknown:
      return true;
    case JobState::Queued:
    case JobState::PreflightOk:
    case JobState::SendStarted:
    case JobState::FailedKnown:
    case JobState::HeldOffline:
      return false;
  }
  return false;
}

// Whether an *ordered* fence actually answered — which is not the same question as
// whether the job finished. A printer with no fence still reaches DoneSoftware; it just
// gets there on grade E, with nothing but a socket write behind the claim
// (docs/sdk-spec.md §5: the SDK never upgrades confidence on its own).
bool fenceCameBack(ConfidenceLevel confidence) noexcept {
  switch (confidence) {
    case ConfidenceLevel::PrintConfirmed:
    case ConfidenceLevel::CutProcessed:
    case ConfidenceLevel::CutFaultFree:
    case ConfidenceLevel::PhysicallyVerified:
      return true;
    case ConfidenceLevel::TransportAccepted:
    case ConfidenceLevel::PrinterHealthy:
      return false;
  }
  return false;
}

// The fence this printer would use, named as the command a support engineer looks up
// six months later. Taken from the core's own table so the agent and the journal never
// disagree about what "printFence" means (docs/techspec.md §3).
const char* fenceName(CompletionMechanism mechanism) noexcept {
  return evidenceFor(mechanism).method;
}

const char* tri(const std::optional<bool>& value, const char* yes, const char* no) {
  if (!value.has_value()) {
    return "unknown";
  }
  return *value ? yes : no;
}

Json statusJson(const DeviceStatus& status) {
  Json out = Json::object({});
  out.set("connected", Json::boolean(status.connected));
  out.set("observed", Json::boolean(status.observed));
  auto flag = [&](const char* name, const std::optional<bool>& value) {
    // A capability nobody established is not the same as one that is absent, so the
    // wire carries null rather than false.
    out.set(name, value.has_value() ? Json::boolean(*value) : Json::null());
  };
  flag("online", status.online);
  flag("coverOpen", status.cover_open);
  flag("paperOut", status.paper_out);
  flag("paperNearEnd", status.paper_near_end);
  flag("cutterError", status.cutter_error);
  flag("recoverableError", status.recoverable_error);
  flag("unrecoverableError", status.unrecoverable_error);
  return out;
}

std::string trimmed(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

HttpServerConfig httpConfigFrom(const AgentConfig& config) {
  HttpServerConfig http;
  http.bind = config.bind;
  http.port = config.port;
  http.workers = std::max<uint32_t>(1, config.workers);
  return http;
}

// "/printers/kitchen/status" → {"printers", "kitchen", "status"}.
//
// Splits the *encoded* path and decodes each segment afterwards, never the other way
// round: keys carry '#' and verification tokens are base 94 over printable ASCII, so a
// segment can legitimately contain an encoded '/'. Decoding first would turn it into a
// route boundary and lose the job.
std::vector<std::string> segments(const std::string& raw_path) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < raw_path.size()) {
    while (pos < raw_path.size() && raw_path[pos] == '/') {
      ++pos;
    }
    size_t end = raw_path.find('/', pos);
    if (end == std::string::npos) {
      end = raw_path.size();
    }
    if (end > pos) {
      out.push_back(percentDecode(raw_path.substr(pos, end - pos)));
    }
    pos = end;
  }
  return out;
}

}  // namespace

std::string PrinterSpec::endpoint() const {
  return "tcp://" + host + ":" + std::to_string(port);
}

// --- Configuration ---------------------------------------------------------------------

bool parseAgentConfig(const Json& json, AgentConfig* out, std::string* error) {
  auto refuse = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (!json.isObject()) {
    return refuse("config must be a JSON object");
  }
  if (const Json* value = json.find("store"); value != nullptr && value->isString()) {
    out->store = value->asString();
  }
  if (const Json* value = json.find("bind"); value != nullptr && value->isString()) {
    out->bind = value->asString();
  }
  if (const Json* value = json.find("port"); value != nullptr && value->isNumber()) {
    const long long port = value->asInt();
    if (port < 0 || port > 65535) {
      return refuse("port out of range");
    }
    out->port = static_cast<uint16_t>(port);
  }
  if (const Json* value = json.find("workers"); value != nullptr && value->isNumber()) {
    out->workers = static_cast<uint32_t>(std::max<long long>(1, value->asInt()));
  }
  if (const Json* value = json.find("waitMs"); value != nullptr && value->isNumber()) {
    out->wait_ms = static_cast<uint32_t>(std::max<long long>(0, value->asInt()));
  }

  // --- M13b: CloudPRNT printers (docs/wire-protocols.md §2) --------------------------
  //
  //   "cloudprnt": [ { "id": "counter", "mac": "00:11:62:aa:bb:cc",
  //                    "mediaTypes": ["application/vnd.star.line"],
  //                    "maxPendingJobs": 32 } ]
  //
  // No host and no port: this printer dials the agent, so its whole configuration is the
  // URL segment it polls, the identity it may claim, and what it can be handed.
  if (const Json* list = json.find("cloudprnt"); list != nullptr) {
    if (!list->isArray()) {
      return refuse("cloudprnt must be an array");
    }
    for (size_t i = 0; i < list->asArray().size(); ++i) {
      const Json& entry = list->asArray()[i];
      const std::string where = "cloudprnt[" + std::to_string(i) + "]";
      if (!entry.isObject()) {
        return refuse(where + " must be an object");
      }
      CloudPrntSpec spec;
      if (const Json* value = entry.find("id"); value != nullptr && value->isString()) {
        spec.id = value->asString();
      }
      if (spec.id.empty()) {
        // The id is the route the printer is configured to poll. Deriving one would mean
        // guessing what somebody typed into a printer's web console.
        return refuse(where + ".id is required: it is the URL segment the printer polls");
      }
      if (const Json* value = entry.find("mac"); value != nullptr && value->isString()) {
        spec.mac = value->asString();
      }
      if (const Json* value = entry.find("mediaTypes");
          value != nullptr && value->isArray()) {
        for (const Json& media : value->asArray()) {
          if (!media.isString() || media.asString().empty()) {
            return refuse(where + ".mediaTypes must be non-empty strings");
          }
          spec.media_types.push_back(media.asString());
        }
      }
      if (const Json* value = entry.find("maxPendingJobs");
          value != nullptr && value->isNumber()) {
        spec.max_pending = static_cast<size_t>(std::max<long long>(1, value->asInt()));
      }
      out->cloudprnt.push_back(std::move(spec));
    }
  }
  // --- end M13b ------------------------------------------------------------------------

  const Json* printers = json.find("printers");
  if (printers == nullptr) {
    return true;
  }
  if (!printers->isArray()) {
    return refuse("printers must be an array");
  }
  for (size_t i = 0; i < printers->asArray().size(); ++i) {
    const Json& entry = printers->asArray()[i];
    const std::string where = "printers[" + std::to_string(i) + "]";
    if (!entry.isObject()) {
      return refuse(where + " must be an object");
    }
    PrinterSpec spec;
    if (const Json* value = entry.find("id"); value != nullptr && value->isString()) {
      spec.id = value->asString();
    }
    const Json* tcp = entry.find("tcp");
    if (tcp == nullptr || !tcp->isObject()) {
      return refuse(where + " needs a tcp object with a host");
    }
    if (const Json* value = tcp->find("host"); value != nullptr && value->isString()) {
      spec.host = value->asString();
    }
    if (spec.host.empty()) {
      return refuse(where + ".tcp.host is required");
    }
    if (const Json* value = tcp->find("port"); value != nullptr && value->isNumber()) {
      const long long port = value->asInt();
      if (port <= 0 || port > 65535) {
        return refuse(where + ".tcp.port out of range");
      }
      spec.port = static_cast<uint16_t>(port);
    }
    if (const Json* value = tcp->find("connectTimeoutMs");
        value != nullptr && value->isNumber()) {
      spec.connect_timeout_ms = static_cast<uint32_t>(std::max<long long>(1, value->asInt()));
    }
    if (const Json* value = entry.find("widthDots");
        value != nullptr && value->isNumber()) {
      spec.width_dots = static_cast<uint32_t>(std::max<long long>(0, value->asInt()));
    }
    if (const Json* value = entry.find("profile");
        value != nullptr && value->isString()) {
      spec.profile = value->asString();
    }
    CapabilityProfile unused;
    if (!profileByName(spec.profile, &unused)) {
      return refuse(where + ".profile is not in the device database: " + spec.profile);
    }
    out->printers.push_back(std::move(spec));
  }
  return true;
}

bool loadAgentConfigFile(const std::string& path, AgentConfig* out,
                         std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr) {
      *error = "cannot read " + path;
    }
    return false;
  }
  std::ostringstream text;
  text << file.rdbuf();
  Json json;
  std::string parse_error;
  if (!dsl::tryParseJson(text.str(), &json, &parse_error)) {
    if (error != nullptr) {
      *error = path + ": " + parse_error;
    }
    return false;
  }
  return parseAgentConfig(json, out, error);
}

// --- Agent -----------------------------------------------------------------------------

Agent::Agent(AgentConfig config)
    : config_(std::move(config)),
      driver_(std::make_unique<PrinterDriver>(
          config_.store.empty() ? StorageConfig::inMemory()
                                : StorageConfig::at(config_.store))),
      server_(httpConfigFrom(config_)),
      foreign_(std::make_shared<ForeignWriters>()) {
  // docs/sdk-spec.md §14: an echo carrying a token this instance never issued is the
  // one-owner invariant being violated. It is recorded and reported, never swallowed —
  // a mis-deployed topology has to be visible from the API.
  //
  // The callback runs on a printer's worker thread and captures only the registry, not
  // the agent: it must never reach for the agent's own mutex, or a thread publishing an
  // event while holding a runtime lock would deadlock against a request holding the
  // agent lock and asking that runtime for its status.
  auto registry = foreign_;
  driver_->subscribeDevices([registry](const std::string& printer_id, DeviceEvent event) {
    if (event == DeviceEvent::ForeignWriterDetected) {
      registry->mark(printer_id);
    }
  });
  started_ = MonotonicClock::now();
}

Agent::~Agent() {
  stop();
  if (driver_) {
    driver_->shutdown();
  }
}

void Agent::setPrinterConfigHook(PrinterConfigHook hook) { hook_ = std::move(hook); }

bool Agent::addPrinter(const PrinterSpec& spec, std::string* error,
                       std::string* assigned_id) {
  auto refuse = [&](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  PrinterSpec effective = spec;
  if (effective.host.empty()) {
    return refuse("a printer needs a tcp host");
  }
  CapabilityProfile profile;
  if (!profileByName(effective.profile, &profile)) {
    return refuse("unknown profile: " + effective.profile);
  }
  if (effective.id.empty()) {
    effective.id = effective.host + ":" + std::to_string(effective.port);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (by_id_.find(effective.id) != by_id_.end()) {
      return refuse("printer id already owned: " + effective.id);
    }
    for (const Owned& entry : printers_) {
      // The single-owner invariant starts at home: two lanes onto one socket would
      // interleave receipts and misroute echoes inside this very process.
      if (!effective.host.empty() && entry.spec.host == effective.host &&
          entry.spec.port == effective.port) {
        return refuse("printer " + entry.spec.id + " already owns " +
                      effective.endpoint());
      }
    }
  }

  PrinterConfig config;
  config.id = effective.id;
  config.profile = profile;
  config.width_dots = effective.width_dots != 0 ? effective.width_dots
                                                : profile.media.printable_width_dots;
  TcpConfig tcp;
  tcp.host = effective.host;
  tcp.port = effective.port;
  tcp.connect_timeout_ms = effective.connect_timeout_ms;
  config.transport = pd::tcp(tcp);
  if (hook_) {
    hook_(effective, &config);
  }
  if (!config.transport) {
    return refuse("no transport for printer " + effective.id);
  }

  std::shared_ptr<Printer> printer = driver_->addPrinter(std::move(config));
  if (!printer) {
    return refuse("could not add printer " + effective.id);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  by_id_[effective.id] = printers_.size();
  printers_.push_back(Owned{effective, printer});
  if (assigned_id != nullptr) {
    *assigned_id = effective.id;
  }
  return true;
}

bool Agent::start(std::string* error) {
  for (const PrinterSpec& spec : config_.printers) {
    if (!addPrinter(spec, error)) {
      return false;
    }
  }
  // --- M13b: CloudPRNT printers --------------------------------------------------------
  // One id namespace for both kinds. A CloudPRNT printer that shadowed a pushed printer's
  // id would make GET /printers ambiguous and POST /jobs route by luck.
  for (const CloudPrntSpec& spec : config_.cloudprnt) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (by_id_.find(spec.id) != by_id_.end()) {
        if (error != nullptr) {
          *error = "printer id already owned: " + spec.id;
        }
        return false;
      }
    }
    if (!cloudprnt_.addPrinter(spec, error)) {
      return false;
    }
  }
  // --- end M13b -------------------------------------------------------------------------
  if (!server_.start([this](const HttpRequest& request) { return handle(request); },
                     error)) {
    return false;
  }
  serving_ = true;
  return true;
}

void Agent::stop() {
  if (serving_) {
    server_.stop();
    serving_ = false;
  }
}

std::shared_ptr<Printer> Agent::lookup(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : printers_[it->second].printer;
}

const Agent::Owned* Agent::owned(const std::string& id) const {
  const auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : &printers_[it->second];
}

// --- Routing ---------------------------------------------------------------------------

HttpResponse Agent::handle(const HttpRequest& request) {
  const std::vector<std::string> path = segments(request.raw_path);
  const std::string& method = request.method;

  if (path.empty()) {
    return fail(404, "not found", request.path);
  }
  if (path[0] == "healthz" && path.size() == 1) {
    if (method != "GET") {
      return fail(405, "method not allowed", method);
    }
    return getHealth();
  }
  if (path[0] == "jobs") {
    if (path.size() == 1) {
      if (method != "POST") {
        return fail(405, "method not allowed", method);
      }
      return postJobs(request);
    }
    if (path.size() == 2) {
      if (method != "GET") {
        return fail(405, "method not allowed", method);
      }
      return getJob(path[1]);
    }
    return fail(404, "not found", request.path);
  }
  if (path[0] == "printers") {
    if (path.size() == 1) {
      if (method == "GET") {
        return getPrinters();
      }
      if (method == "POST") {
        return postPrinters(request);
      }
      return fail(405, "method not allowed", method);
    }
    if (path.size() == 2 && method == "GET") {
      return getPrinterStatus(path[1], request);
    }
    if (path.size() == 3 && path[2] == "status" && method == "GET") {
      return getPrinterStatus(path[1], request);
    }
    // M14 — docs/cash-drawer.md. POST because it energises a solenoid; the answer is a
    // drawer state and never a boolean.
    if (path.size() == 3 && path[2] == "drawer") {
      if (method != "POST") {
        return fail(405, "method not allowed", method);
      }
      return postPrinterDrawer(path[1], request);
    }
    // M15 — docs/api.md §15. POST because it consumes paper; the answer is the whole
    // detection report plus the ordinary tri-state job result.
    if (path.size() == 3 && path[2] == "self-test") {
      if (method != "POST") {
        return fail(405, "method not allowed", method);
      }
      return postPrinterSelfTest(path[1], request);
    }
    return fail(404, "not found", request.path);
  }
  // M15 — docs/api.md §15. Not under /printers, because it is what runs BEFORE there are
  // any: it takes a subnet and returns candidates, and adding one is still a deliberate
  // POST /printers.
  if (path[0] == "autodetect" && path.size() == 1) {
    if (method != "POST") {
      return fail(405, "method not allowed", method);
    }
    return postAutoDetect(request);
  }
  // --- M13b: CloudPRNT (docs/wire-protocols.md §2) -------------------------------------
  //
  //   GET    /cloudprnt                     every polling printer this agent serves
  //   POST   /cloudprnt/<id>                the printer's poll
  //   GET    /cloudprnt/<id>                the printer's job download (idempotent)
  //   DELETE /cloudprnt/<id>                the printer's confirmation (the only delete)
  //   POST   /cloudprnt/<id>/jobs           an application hands bytes in
  //   GET    /cloudprnt/<id>/jobs           what is queued and what was confirmed
  //   GET    /cloudprnt/<id>/jobs/<token>   one job's evidence document
  //
  // The printer-facing triple all lands on the same path because that is the contract: a
  // CloudPRNT printer is configured with one URL and varies the method, so `/cloudprnt/<id>`
  // is what goes into its web console and the three verbs are the whole protocol.
  if (path[0] == "cloudprnt") {
    return cloudPrntRoute(path, request);
  }
  // --- end M13b -------------------------------------------------------------------------
  return fail(404, "not found", request.path);
}

HttpResponse Agent::getHealth() const {
  Json out = Json::object({});
  out.set("ok", Json::boolean(true));
  std::lock_guard<std::mutex> lock(mutex_);
  out.set("printers", Json::number(static_cast<double>(printers_.size())));
  out.set("jobs", Json::number(static_cast<double>(driver_->store().size())));
  out.set("store", config_.store.empty() ? Json::null()
                                         : Json::string(config_.store));
  out.set("durable", Json::boolean(driver_->store().persistent()));
  // docs/api.md §14: the two-character prefix every verification identifier this
  // instance issues carries. It is what names the owner on the paper itself.
  out.set("instanceNonce", Json::string(driver_->instanceNonce()));
  out.set("recoveredJobs",
          Json::number(static_cast<double>(driver_->store().recoveredCount())));
  out.set("foreignWriterDetected", Json::boolean(foreign_->any()));
  // --- M13b: CloudPRNT ------------------------------------------------------------------
  // Counted separately from `printers` above: these are not lanes this process owns a
  // socket to, and folding them into one number would overstate what the agent can write
  // to. `cloudprntAwaitingConfirmation` is the number of jobs a printer has been offered
  // or has downloaded and not yet confirmed — the retention rule's backlog, in one figure.
  out.set("cloudprntPrinters",
          Json::number(static_cast<double>(cloudprnt_.printerCount())));
  out.set("cloudprntAwaitingConfirmation",
          Json::number(static_cast<double>(cloudprnt_.awaitingConfirmation())));
  // --- end M13b -------------------------------------------------------------------------
  const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                          MonotonicClock::now() - started_)
                          .count();
  out.set("uptimeMs", Json::number(static_cast<double>(uptime)));
  return ok(200, out);
}

Json Agent::printerJson(const Owned& entry) const {
  const CapabilityProfile profile = entry.printer->profile();
  Json out = Json::object({});
  out.set("id", Json::string(entry.printer->id()));
  out.set("transport", Json::string(entry.spec.host.empty()
                                        ? std::string("scripted")
                                        : entry.spec.endpoint()));
  out.set("widthDots", Json::number(static_cast<double>(entry.printer->widthDots())));
  out.set("profile", Json::string(profile.name));
  out.set("completion", Json::string(fenceName(profile.completion)));
  out.set("grade", Json::string(gradeLetter(profile.evidence().grade)));
  out.set("probed", Json::boolean(profile.probed));
  out.set("foreignWriterDetected",
          Json::boolean(foreign_->detected(entry.printer->id())));
  out.set("status", statusJson(entry.printer->status()));
  return out;
}

HttpResponse Agent::getPrinters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Json list = Json::array({});
  for (const Owned& entry : printers_) {
    list.push(printerJson(entry));
  }
  // --- M13b: CloudPRNT ------------------------------------------------------------------
  // The polling printers belong in this list — an operator asking what this agent prints
  // to means all of them — but they carry `kind: "cloudprnt"` and their own field set,
  // because there is no endpoint to dial and no fence for the engine to hold. Their device
  // snapshot goes through the same statusJson() the pushed printers use, so "paperOut"
  // means the same thing in both entries: something told us, and this is what it said.
  for (const std::string& id : cloudprnt_.printerIds()) {
    Json entry = cloudprnt_.printerJson(id);
    entry.set("status", statusJson(cloudprnt_.deviceStatus(id)));
    list.push(std::move(entry));
  }
  // --- end M13b -------------------------------------------------------------------------
  Json out = Json::object({});
  out.set("printers", std::move(list));
  return ok(200, out);
}

HttpResponse Agent::getPrinterStatus(const std::string& id,
                                     const HttpRequest& request) {
  std::shared_ptr<Printer> printer = lookup(id);
  if (!printer) {
    // --- M13b: CloudPRNT ------------------------------------------------------------------
    // A polling printer answers here too, from its last poll. `?refresh=1` cannot be
    // honoured — there is no socket to send DLE EOT down, and the printer speaks only when
    // it decides to — so the response says so rather than pretending the snapshot is fresh.
    if (cloudprnt_.known(id)) {
      Json entry = cloudprnt_.printerJson(id);
      entry.set("status", statusJson(cloudprnt_.deviceStatus(id)));
      entry.set("refreshSupported", Json::boolean(false));
      return ok(200, entry);
    }
    // --- end M13b -------------------------------------------------------------------------
    return fail(404, "unknown printer", id);
  }
  bool refresh_asked = false;
  const std::string refresh = request.queryValue("refresh", &refresh_asked);
  if (refresh_asked && refresh != "0" && refresh != "false") {
    // Queues a DLE EOT 1-4 round trip behind any active job. Real-time status is a
    // diagnostic, never a completion fence (docs/techspec.md §3.3).
    printer->refreshStatus(std::chrono::milliseconds(2000));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const Owned* entry = owned(id);
  if (entry == nullptr) {
    return fail(404, "unknown printer", id);
  }
  return ok(200, printerJson(*entry));
}

HttpResponse Agent::postPrinters(const HttpRequest& request) {
  Json json;
  std::string parse_error;
  if (!dsl::tryParseJson(request.body, &json, &parse_error)) {
    return fail(400, "malformed JSON", parse_error);
  }
  AgentConfig parsed;
  Json wrapper = Json::object({});
  Json list = Json::array({});
  list.push(json);
  wrapper.set("printers", std::move(list));
  std::string config_error;
  if (!parseAgentConfig(wrapper, &parsed, &config_error) || parsed.printers.empty()) {
    return fail(400, "invalid printer", config_error);
  }
  std::string error;
  std::string assigned;
  if (!addPrinter(parsed.printers.front(), &error, &assigned)) {
    // A duplicate id or a second lane onto the same socket: both are the single-owner
    // invariant, and both are a conflict rather than a bad request.
    const bool conflict = error.find("already") != std::string::npos;
    return fail(conflict ? 409 : 400, "printer rejected", error);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const Owned* entry = owned(assigned);
  if (entry == nullptr) {
    return fail(500, "printer vanished after being added");
  }
  return ok(201, printerJson(*entry));
}

// --- M14: cash drawer (docs/cash-drawer.md) ----------------------------------------------
//
// The endpoint answers with the same honesty the C++ and C surfaces do, because it is
// the same call underneath. Three things it deliberately does not do:
//
//   - it never reports `{ "opened": true }`. `state` is one of the eight members of the
//     drawer state machine, and KICK_SENT_UNVERIFIED is a real answer rather than a
//     softer success — through a print server the pulse travels forward while the
//     sensor response does not come back, and an operator needs those apart;
//   - it never fires an unclassified port. A 6P6C socket nobody has classified is
//     refused with 409 and zero bytes written, which is the giant-letters rule of the
//     document expressed as a status code;
//   - it never guesses a polarity. Until the switch has been calibrated on this
//     printer, `needsCalibration` is true and the state stays UNKNOWN.

namespace {

// SCREAMING_SNAKE on the wire, matching the document's own spelling of the states
// ("OPEN_VERIFIED", "KICK_SENT_UNVERIFIED") rather than the core's CamelCase.
const char* drawerStateWire(DrawerState state) {
  switch (state) {
    case DrawerState::Closed: return "CLOSED";
    case DrawerState::Open: return "OPEN";
    case DrawerState::Opening: return "OPENING";
    case DrawerState::KickSentUnverified: return "KICK_SENT_UNVERIFIED";
    case DrawerState::OpenVerified: return "OPEN_VERIFIED";
    case DrawerState::FailedToOpen: return "FAILED_TO_OPEN";
    case DrawerState::NoSensor: return "NO_SENSOR";
    case DrawerState::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

Json drawerCapabilitiesJson(const DrawerCapabilities& drawer) {
  Json electrical = Json::object({});
  electrical.set("standard", Json::string(to_string(drawer.electrical.standard)));
  electrical.set("voltage", Json::number(drawer.electrical.voltage));
  electrical.set("maxCurrentMa", Json::number(drawer.electrical.max_current_ma));
  electrical.set("channels", Json::number(drawer.electrical.channel_count));
  electrical.set("sensorPin", Json::number(drawer.electrical.sensor_pin));

  Json kick = Json::object({});
  kick.set("method", Json::string(to_string(drawer.kick.method)));
  kick.set("defaultPulseMs", Json::number(drawer.kick.default_pulse_ms));
  kick.set("maxPulseMs", Json::number(drawer.kick.max_pulse_ms));
  kick.set("cooldownMs", Json::number(drawer.kick.cooldown_ms));
  kick.set("canKickDuringPrint", Json::boolean(drawer.kick.can_kick_during_print));

  Json status = Json::object({});
  status.set("available", Json::boolean(drawer.status.available));
  status.set("method", Json::string(to_string(drawer.status.method)));
  status.set("sharedBetweenDrawers",
             Json::boolean(drawer.status.shared_between_drawers));
  status.set("calibrated", Json::boolean(drawer.status.polarity.calibrated));
  status.set("highMeansOpen", Json::boolean(drawer.status.polarity.high_means_open));

  Json evidence = Json::object({});
  // Two columns, never one flag: the XP-S260M's 24 V / 1 A output is documented while
  // nothing published proves its pulse command.
  evidence.set("electrical", Json::string(to_string(drawer.evidence.electrical)));
  evidence.set("commands", Json::string(to_string(drawer.evidence.commands)));
  evidence.set("documented", Json::boolean(drawer.evidence.documented()));
  evidence.set("probed", Json::boolean(drawer.evidence.probed()));

  Json out = Json::object({});
  out.set("present", Json::boolean(drawer.present));
  out.set("kickable", Json::boolean(drawer.kickable()));
  out.set("electrical", std::move(electrical));
  out.set("kick", std::move(kick));
  out.set("status", std::move(status));
  Json port = Json::object({});
  port.set("sharedWithBuzzer", Json::boolean(drawer.port.shared_with_buzzer));
  out.set("port", std::move(port));
  out.set("evidence", std::move(evidence));
  if (!drawer.note.empty()) {
    out.set("note", Json::string(drawer.note));
  }
  return out;
}

}  // namespace

HttpResponse Agent::postPrinterDrawer(const std::string& id, const HttpRequest& request) {
  std::shared_ptr<Printer> printer = lookup(id);
  if (!printer) {
    return fail(404, "unknown printer", id);
  }

  DrawerRequest wanted;
  const std::string body = trimmed(request.body);
  if (!body.empty()) {
    Json json;
    std::string parse_error;
    if (!dsl::tryParseJson(body, &json, &parse_error)) {
      return fail(400, "malformed JSON", parse_error);
    }
    if (!json.isObject()) {
      return fail(400, "body must be a JSON object");
    }
    if (const Json* channel = json.find("channel"); channel != nullptr) {
      if (!channel->isNumber() || channel->asInt() < 1 || channel->asInt() > 255) {
        return fail(400, "channel must be 1 or 2");
      }
      wanted.channel = static_cast<uint8_t>(channel->asInt());
    }
    if (const Json* pulse = json.find("pulseMs"); pulse != nullptr) {
      if (!pulse->isNumber() || pulse->asInt() < 1 || pulse->asInt() > 5000) {
        return fail(400, "pulseMs must be 1..5000");
      }
      wanted.pulse_ms = static_cast<uint16_t>(pulse->asInt());
    }
  }

  const DrawerCapabilities caps = printer->drawerCapabilities();
  if (!caps.kickable()) {
    // Refused before a byte is written. 409 rather than 400 because nothing about the
    // request is wrong — the hardware, or what is known about it, is what forbids this.
    Json out = Json::object({});
    out.set("error", Json::string(
        !caps.present
            ? "this printer has no drawer port"
            : (!caps.electricalKnown()
                   ? "the drawer port's electrical standard is unknown: "
                     "RJ11/RJ12-looking connectors are not a universal standard, and "
                     "an unclassified port is never energised"
                   : "this drawer's kick method is not one this engine drives")));
    out.set("printerId", Json::string(printer->id()));
    out.set("drawer", drawerCapabilitiesJson(caps));
    return ok(409, out);
  }

  const DrawerOpenResult result = printer->openDrawer(wanted);

  Json out = Json::object({});
  out.set("printerId", Json::string(printer->id()));
  out.set("state", Json::string(drawerStateWire(result.state)));
  out.set("previousState", Json::string(drawerStateWire(result.previous_state)));
  out.set("channel", Json::number(result.channel));
  out.set("pulseMs", Json::number(result.pulse_ms));
  out.set("elapsedMs", Json::number(result.elapsed_ms));
  // The two facts a caller has to be able to branch on without parsing prose.
  out.set("verified", Json::boolean(result.state == DrawerState::OpenVerified));
  out.set("needsCalibration",
          Json::boolean(!printer->drawerPolarity().calibrated));
  out.set("drawer", drawerCapabilitiesJson(printer->drawerCapabilities()));

  // 200 for a settled answer, 202 for one nothing could confirm — the same split
  // POST /jobs uses between a completed job and a fence still outstanding.
  const int status = result.state == DrawerState::KickSentUnverified ? 202 : 200;
  return ok(status, out);
}

// --- M15: self-test and auto-detection (docs/api.md §15) ---------------------------------

namespace {

Json detectionSummaryJson(const DetectionSummary& summary) {
  Json identity = Json::object({});
  identity.set("vendor", Json::string(summary.identity.vendor));
  identity.set("model", Json::string(summary.identity.model));
  identity.set("firmware", Json::string(summary.identity.firmware));
  identity.set("serial", Json::string(summary.identity.serial));
  // Never a bare vendor string: GS I is what the firmware chose to say, and at least
  // one family ships answering as somebody else's model.
  identity.set("trusted", Json::boolean(summary.identity.trusted));
  identity.set("confidencePercent",
               Json::number(static_cast<double>(summary.identity.confidence_percent)));
  identity.set("impersonationSuspected",
               Json::boolean(summary.identity.impersonation_suspected));
  identity.set("fresh", Json::boolean(summary.identity_fresh));
  Json signals = Json::array({});
  for (const std::string& signal : summary.identity.signals) {
    signals.push(Json::string(signal));
  }
  identity.set("signals", std::move(signals));

  Json media = Json::object({});
  media.set("nominalPaperMm", Json::number(summary.nominal_paper_mm));
  media.set("printableWidthDots", Json::number(summary.printable_width_dots));
  media.set("charsPerLine", Json::number(summary.chars_per_line));
  media.set("dpi", Json::number(summary.dpi));

  Json completion = Json::object({});
  completion.set("mechanism", Json::string(to_string(summary.completion)));
  completion.set("gradeCeiling", Json::string(gradeLetter(summary.grade_ceiling)));
  completion.set("authority", Json::string(to_string(summary.authority)));
  completion.set("method", Json::string(summary.method));
  completion.set("provenance", Json::string(to_string(summary.completion_provenance)));

  Json drawer = Json::object({});
  drawer.set("present", Json::boolean(summary.drawer_present));
  drawer.set("kickable", Json::boolean(summary.drawer_kickable));
  drawer.set("standard", Json::string(to_string(summary.drawer_standard)));
  drawer.set("voltage", Json::number(summary.drawer_voltage));
  drawer.set("electricalProvenance",
             Json::string(to_string(summary.drawer_electrical_provenance)));
  drawer.set("commandsProvenance",
             Json::string(to_string(summary.drawer_commands_provenance)));

  Json degradations = Json::array({});
  for (const std::string& line : summary.degradations) {
    degradations.push(Json::string(line));
  }

  Json out = Json::object({});
  out.set("endpoint", Json::string(summary.endpoint));
  out.set("identity", std::move(identity));
  out.set("profile", Json::string(summary.profile_id));
  out.set("selectedBy", Json::string(to_string(summary.selection)));
  out.set("media", std::move(media));
  out.set("completion", std::move(completion));
  out.set("drawer", std::move(drawer));
  out.set("degradations", std::move(degradations));
  out.set("provenanceSummary", Json::string(summary.provenanceSummary()));
  return out;
}

}  // namespace

HttpResponse Agent::postPrinterSelfTest(const std::string& id,
                                        const HttpRequest& request) {
  std::shared_ptr<Printer> printer = lookup(id);
  if (!printer) {
    return fail(404, "unknown printer", id);
  }

  SelfTestOptions options;
  const std::string body = trimmed(request.body);
  if (!body.empty()) {
    Json json;
    std::string parse_error;
    if (!dsl::tryParseJson(body, &json, &parse_error)) {
      return fail(400, "malformed JSON", parse_error);
    }
    if (!json.isObject()) {
      return fail(400, "body must be a JSON object");
    }
    if (const Json* key = json.find("key"); key != nullptr) {
      if (!key->isString()) {
        return fail(400, "key must be a string");
      }
      options.key = key->asString();
    }
    if (const Json* refresh = json.find("refreshIdentity"); refresh != nullptr) {
      if (!refresh->isBool()) {
        return fail(400, "refreshIdentity must be a boolean");
      }
      options.refresh_identity = refresh->asBool();
    }
    if (const Json* barcode = json.find("barcode"); barcode != nullptr) {
      if (!barcode->isBool()) {
        return fail(400, "barcode must be a boolean");
      }
      options.barcode = barcode->asBool();
    }
  }

  const SelfTestResult result = printer->selfTest(options);

  Json out = Json::object({});
  out.set("printerId", Json::string(printer->id()));
  out.set("key", Json::string(result.key));
  out.set("token", Json::string(result.print_token));
  out.set("detection", detectionSummaryJson(result.detection));
  // The ticket exactly as it was laid out — the same layout that produced the bytes.
  Json lines = Json::array({});
  for (const std::string& line : result.ticket_lines) {
    lines.push(Json::string(line));
  }
  out.set("ticket", std::move(lines));

  Json job = Json::object({});
  job.set("outcome", Json::string(to_string(result.result.outcome)));
  job.set("confidence", Json::string(to_string(result.result.confidence)));
  job.set("grade", Json::string(gradeLetter(result.result.grade)));
  job.set("authority", Json::string(to_string(result.result.authority)));
  job.set("method", Json::string(result.result.method));
  if (result.result.reason != FailureReason::None) {
    job.set("reason", Json::string(to_string(result.result.reason)));
  }
  if (result.job) {
    job.set("id", Json::string(result.job->id()));
  }
  out.set("result", std::move(job));

  // The same three-way split every job answer uses: 200 for a settled Done, 409 for a
  // settled Failed, 202 for an Unknown nothing could confirm. A self-test that came back
  // Unknown is not a success and must not be reported as one.
  switch (result.result.outcome) {
    case JobOutcome::Done: return ok(200, out);
    case JobOutcome::Failed: return ok(409, out);
    case JobOutcome::Unknown: break;
  }
  return ok(202, out);
}

HttpResponse Agent::postAutoDetect(const HttpRequest& request) {
  AutoDetectOptions options;
  const std::string body = trimmed(request.body);
  if (!body.empty()) {
    Json json;
    std::string parse_error;
    if (!dsl::tryParseJson(body, &json, &parse_error)) {
      return fail(400, "malformed JSON", parse_error);
    }
    if (!json.isObject()) {
      return fail(400, "body must be a JSON object");
    }
    if (const Json* cidr = json.find("cidr"); cidr != nullptr) {
      if (!cidr->isString()) {
        return fail(400, "cidr must be a string");
      }
      options.subnet_cidr = cidr->asString();
    }
    if (const Json* endpoints = json.find("endpoints"); endpoints != nullptr) {
      if (!endpoints->isArray()) {
        return fail(400, "endpoints must be an array of strings");
      }
      for (const Json& entry : endpoints->asArray()) {
        if (!entry.isString()) {
          return fail(400, "endpoints must be an array of strings");
        }
        options.endpoints.push_back(entry.asString());
      }
    }
    if (const Json* port = json.find("port"); port != nullptr) {
      if (!port->isNumber() || port->asInt() < 1 || port->asInt() > 65535) {
        return fail(400, "port must be 1..65535");
      }
      options.port = static_cast<uint16_t>(port->asInt());
    }
    if (const Json* concurrency = json.find("concurrency"); concurrency != nullptr) {
      if (!concurrency->isNumber() || concurrency->asInt() < 1 ||
          concurrency->asInt() > 256) {
        return fail(400, "concurrency must be 1..256");
      }
      options.concurrency = static_cast<uint32_t>(concurrency->asInt());
    }
    if (const Json* connect = json.find("connectTimeoutMs"); connect != nullptr) {
      if (!connect->isNumber() || connect->asInt() < 1) {
        return fail(400, "connectTimeoutMs must be positive");
      }
      options.connect_timeout_ms = static_cast<uint32_t>(connect->asInt());
    }
    if (const Json* probe = json.find("probeUnknown"); probe != nullptr) {
      if (!probe->isBool()) {
        return fail(400, "probeUnknown must be a boolean");
      }
      options.probe_unknown = probe->asBool();
    }
  }

  std::vector<DetectedPrinter> found;
  try {
    found = driver_->autoDetect(options);
  } catch (const DiscoveryError& error) {
    // A malformed CIDR, or one wider than /16 — a mistyped subnet rather than a venue.
    return fail(400, "cannot sweep that subnet", error.what());
  }

  Json printers = Json::array({});
  for (const DetectedPrinter& one : found) {
    Json entry = Json::object({});
    entry.set("endpoint", Json::string(one.endpoint));
    entry.set("host", Json::string(one.host));
    entry.set("port", Json::number(one.port));
    entry.set("status", Json::string(to_string(one.status)));
    entry.set("portOpen", Json::boolean(one.port_open));
    entry.set("fromCache", Json::boolean(one.from_cache));
    entry.set("dleEot", Json::string(DiscoveredDevice{one.host, one.port, one.port_open,
                                                      one.dle_eot_response}
                                          .responseHex()));
    entry.set("summary", detectionSummaryJson(one.summary));
    printers.push(std::move(entry));
  }

  Json out = Json::object({});
  out.set("printers", std::move(printers));
  out.set("count", Json::number(static_cast<double>(found.size())));
  // Stated on every response, not buried in documentation: this endpoint may not print,
  // so the fences it found were asked out of an empty buffer and their provenance was
  // deliberately not promoted (docs/api.md §15).
  out.set("printed", Json::boolean(false));
  out.set("note",
          Json::string("nothing printed and nothing fired: the printless probe subset "
                       "proves a completion command exists, not that its answer waits "
                       "for paper. Promotion needs the printing probe or a real job."));
  return ok(200, out);
}

// --- Jobs --------------------------------------------------------------------------------

Json Agent::evidenceJson(const JobRecord& record, const std::shared_ptr<PrintJob>& job,
                         bool deduped) const {
  const Owned* entry = owned(record.printer_id);
  const CapabilityProfile profile =
      entry != nullptr ? entry->printer->profile() : generic_escpos();
  const DeviceStatus status =
      entry != nullptr ? entry->printer->status() : DeviceStatus{};

  Json evidence = Json::object({});
  evidence.set("transportAccepted", Json::boolean(bytesLeftTheHost(record.state)));
  evidence.set("printFence", Json::string(fenceName(profile.completion)));
  evidence.set("fenceResponse", Json::boolean(fenceCameBack(record.confidence)));
  // Cutter and paper come from the device's own status, never from the fact that a cut
  // command was sent: a processed cut is not a fault-free cut (docs/techspec.md §3.1).
  evidence.set("cutterStatus",
               Json::string(record.reason == FailureReason::CutterFault
                                ? "fault"
                                : tri(status.cutter_error, "fault", "clear")));
  const char* paper = "unknown";
  if (status.paper_out.has_value() && *status.paper_out) {
    paper = "out";
  } else if (status.paper_near_end.has_value() && *status.paper_near_end) {
    paper = "nearEnd";
  } else if (status.paper_out.has_value()) {
    paper = "ok";
  }
  evidence.set("paperStatus", Json::string(paper));

  Json out = Json::object({});
  out.set("job", Json::string(record.id));
  out.set("state", Json::string(to_string(record.state)));
  out.set("evidence", std::move(evidence));
  out.set("grade", Json::string(gradeLetter(record.grade)));
  out.set("authority", Json::string(to_string(record.authority)));
  out.set("method", Json::string(record.method));
  out.set("token", Json::string(record.print_token));

  // Everything below is context, not evidence: the caller's own key, the lane it ran
  // on, and the tri-state outcome that must never collapse into a boolean.
  out.set("key", Json::string(record.key));
  out.set("printerId", Json::string(record.printer_id));
  out.set("attempt", Json::number(static_cast<double>(record.attempt)));
  out.set("confidence", Json::string(to_string(record.confidence)));
  out.set("cutToken", Json::string(record.cut_token));
  out.set("terminal", Json::boolean(record.isTerminal()));
  out.set("deduped", Json::boolean(deduped));
  out.set("recovered", Json::boolean(record.recovered));
  if (record.reason != FailureReason::None) {
    out.set("reason", Json::string(to_string(record.reason)));
  }
  if (job && job->isTerminal()) {
    out.set("outcome", Json::string(to_string(job->result().outcome)));
  } else if (record.isTerminal()) {
    out.set("outcome", Json::string(record.state == JobState::DoneSoftware ||
                                            record.state == JobState::PhysicallyVerified
                                        ? "Done"
                                        : record.state == JobState::FailedKnown
                                              ? "Failed"
                                              : "Unknown"));
  }
  return out;
}

HttpResponse Agent::getJob(const std::string& reference) {
  std::shared_ptr<PrintJob> job = driver_->findJob(reference);
  if (!job) {
    // docs/api.md §14: paper → job. Somebody holding a receipt has the four-character
    // V: code and nothing else, so the token resolves here too.
    job = driver_->jobByToken(reference);
  }
  std::optional<JobRecord> record;
  if (job) {
    record = driver_->store().findById(job->id());
  }
  if (!record.has_value()) {
    record = driver_->store().findById(reference);
  }
  if (!record.has_value()) {
    record = driver_->store().findByKey(reference);
  }
  if (!record.has_value()) {
    return fail(404, "unknown job", reference);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return ok(200, evidenceJson(*record, job, false));
}

HttpResponse Agent::postJobs(const HttpRequest& request) {
  Json json;
  std::string parse_error;
  if (!dsl::tryParseJson(request.body, &json, &parse_error)) {
    return fail(400, "malformed JSON", parse_error);
  }
  if (!json.isObject()) {
    return fail(400, "body must be a JSON object");
  }

  const Json* printer_id = json.find("printerId");
  if (printer_id == nullptr || !printer_id->isString() ||
      printer_id->asString().empty()) {
    return fail(400, "printerId is required");
  }
  std::shared_ptr<Printer> printer = lookup(printer_id->asString());
  if (!printer) {
    return fail(404, "unknown printer", printer_id->asString());
  }

  std::string key;
  if (const Json* value = json.find("key"); value != nullptr && value->isString()) {
    key = trimmed(value->asString());
  }

  // Fleet-wide dedupe, before anything is built: this driver's journal is the only one
  // in the deployment, so the same order key from two tills resolves to one job
  // (docs/sdk-spec.md §6 and §14). Re-submitting returns the existing job and prints
  // nothing.
  if (!key.empty()) {
    if (std::shared_ptr<PrintJob> existing = driver_->findJob(key)) {
      const std::optional<JobRecord> record = driver_->store().findById(existing->id());
      if (record.has_value()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ok(200, evidenceJson(*record, existing, true));
      }
    }
  }

  const Json* payload = json.find("payload");
  if (payload == nullptr || !payload->isObject()) {
    return fail(400, "payload is required");
  }

  JobOptions options;
  options.key = key;
  const CapabilityProfile profile = printer->profile();
  uint32_t wait_ms = config_.wait_ms;

  if (const Json* value = json.find("options"); value != nullptr && value->isObject()) {
    if (const Json* cut = value->find("cut"); cut != nullptr && cut->isString()) {
      const std::string& text = cut->asString();
      if (text == "none") {
        options.cut = CutSetting::None;
      } else if (text == "partial") {
        options.cut = CutSetting::Partial;
      } else if (text == "full") {
        options.cut = CutSetting::Full;
      } else if (text == "profile") {
        options.cut = CutSetting::Profile;
      } else {
        return fail(400, "options.cut must be profile, partial, full or none", text);
      }
    }
    if (const Json* drawer = value->find("openDrawer"); drawer != nullptr) {
      options.open_drawer = drawer->truthy();
    }
    if (const Json* preflight = value->find("preflight");
        preflight != nullptr && preflight->isString()) {
      if (preflight->asString() == "skip") {
        options.preflight = PreflightMode::Skip;
      } else if (preflight->asString() != "strict") {
        return fail(400, "options.preflight must be strict or skip",
                    preflight->asString());
      }
    }
    if (const Json* timeout = value->find("timeoutMs");
        timeout != nullptr && timeout->isNumber()) {
      options.timeout_ms = static_cast<uint32_t>(std::max<long long>(0, timeout->asInt()));
    }
    if (const Json* top = value->find("topFeedDots");
        top != nullptr && top->isNumber()) {
      options.top_feed_dots =
          static_cast<uint16_t>(std::clamp<long long>(top->asInt(), 0, 0xFFFF));
    }
    if (const Json* bottom = value->find("bottomFeedDots");
        bottom != nullptr && bottom->isNumber()) {
      options.bottom_feed_dots =
          static_cast<uint16_t>(std::clamp<long long>(bottom->asInt(), 0, 0xFFFF));
    }
    if (const Json* vid = value->find("verificationId"); vid != nullptr) {
      options.print_verification_id = vid->truthy();
    }
    if (const Json* wait = value->find("waitMs"); wait != nullptr && wait->isNumber()) {
      wait_ms = static_cast<uint32_t>(std::clamp<long long>(wait->asInt(), 0, 300000));
    }
  }

  Payload built = Payload::raw({});
  Json render_report;
  bool have_render_report = false;

  if (const Json* text = payload->find("text"); text != nullptr && text->isString()) {
    escpos::Encoder encoder;
    encoder.selectCodePage(profile.code_page);
    const std::string& content = text->asString();
    size_t pos = 0;
    while (pos <= content.size()) {
      size_t end = content.find('\n', pos);
      if (end == std::string::npos) {
        end = content.size();
      }
      encoder.line(content.substr(pos, end - pos));
      if (end == content.size()) {
        break;
      }
      pos = end + 1;
    }
    built = Payload::document(encoder);
  } else if (const Json* raster = payload->find("rasterBase64");
             raster != nullptr && raster->isString()) {
    const Json* width = payload->find("width");
    const Json* height = payload->find("height");
    if (width == nullptr || !width->isNumber() || height == nullptr ||
        !height->isNumber()) {
      return fail(400, "rasterBase64 needs width and height in pixels");
    }
    RasterPayload gray;
    gray.width = static_cast<uint32_t>(std::max<long long>(0, width->asInt()));
    gray.height = static_cast<uint32_t>(std::max<long long>(0, height->asInt()));
    if (!decodeBase64(raster->asString(), &gray.gray)) {
      return fail(400, "rasterBase64 is not valid base64");
    }
    const uint64_t expected =
        static_cast<uint64_t>(gray.width) * static_cast<uint64_t>(gray.height);
    if (expected == 0 || gray.gray.size() != expected) {
      return fail(400, "rasterBase64 must be width*height bytes of 8-bit grayscale",
                  std::to_string(gray.gray.size()) + " bytes for " +
                      std::to_string(gray.width) + "x" + std::to_string(gray.height));
    }
    built = Payload::raster(std::move(gray));
  } else if (const Json* tmpl = payload->find("dslTemplate"); tmpl != nullptr) {
    Json document_json;
    if (tmpl->isString()) {
      std::string document_error;
      if (!dsl::tryParseJson(tmpl->asString(), &document_json, &document_error)) {
        return fail(400, "dslTemplate is not valid JSON", document_error);
      }
    } else if (tmpl->isObject()) {
      document_json = *tmpl;
    } else {
      return fail(400, "dslTemplate must be an object or a JSON string");
    }
    try {
      const dsl::Document document = dsl::parseDocument(document_json);
      const Json* model = payload->find("model");
      const dsl::BindOutcome bound =
          dsl::bind(document, model != nullptr ? *model : Json::object({}));
      dsl::RenderOptions render_options;
      render_options.profile = dsl::RenderProfile::from(profile, printer->widthDots());
      const dsl::RenderOutput rendered = dsl::render(bound.document, render_options);

      dsl::RenderReport report = bound.report;
      report.append(rendered.report);
      render_report = report.toJson();
      have_render_report = true;

      // docs/receipt-dsl.md "Cut control" and "Margins": the document declares them,
      // the engine applies them, and an explicit JobOptions value from the caller still
      // wins. The blade-clearance floor is unconditional either way.
      if (rendered.requested_cut.has_value() && options.cut == CutSetting::Profile) {
        switch (*rendered.requested_cut) {
          case dsl::CutRequest::None: options.cut = CutSetting::None; break;
          case dsl::CutRequest::Partial: options.cut = CutSetting::Partial; break;
          case dsl::CutRequest::Full: options.cut = CutSetting::Full; break;
          case dsl::CutRequest::Profile: break;
        }
      }
      if (rendered.requested_margins.top_dots.has_value() && options.top_feed_dots == 0) {
        options.top_feed_dots = static_cast<uint16_t>(
            std::min<uint32_t>(*rendered.requested_margins.top_dots, 0xFFFF));
      }
      if (rendered.requested_margins.bottom_dots.has_value() &&
          options.bottom_feed_dots == 0) {
        options.bottom_feed_dots = static_cast<uint16_t>(
            std::min<uint32_t>(*rendered.requested_margins.bottom_dots, 0xFFFF));
      }
      built = Payload::document(rendered.bytes(), rendered.codePage());
    } catch (const std::exception& error) {
      return fail(400, "dslTemplate could not be rendered", error.what());
    }
  } else {
    return fail(400, "payload needs text, rasterBase64 or dslTemplate");
  }

  std::shared_ptr<PrintJob> job = printer->print(std::move(built), options);
  if (!job) {
    return fail(500, "the engine returned no job");
  }

  const bool terminal =
      wait_ms == 0
          ? job->isTerminal()
          : job->result(std::chrono::milliseconds(wait_ms)).has_value();

  std::optional<JobRecord> record = driver_->store().findById(job->id());
  if (!record.has_value()) {
    return fail(500, "the job left no journal record", job->id());
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Json body = evidenceJson(*record, job, false);
  if (have_render_report) {
    body.set("render", std::move(render_report));
  }
  // 202 means the fence has not answered yet — the job is still the agent's, and
  // GET /jobs/<key> will say how it ended. It is never a failure and never a retry cue.
  return ok(terminal ? 201 : 202, body);
}

// --- M13b: CloudPRNT routes (docs/wire-protocols.md §2) -----------------------------------
//
// The printer-facing triple is deliberately un-authenticated and shaped exactly as the
// document specifies, because the client is firmware: it will send what it sends, and
// anything this end invents it cannot be told about. The application-facing routes live
// under the same prefix so one printer's whole surface is one subtree.
HttpResponse Agent::cloudPrntRoute(const std::vector<std::string>& path,
                                   const HttpRequest& request) {
  const std::string& method = request.method;
  if (path.size() == 1) {
    if (method != "GET") {
      return fail(405, "method not allowed", method);
    }
    return ok(200, cloudprnt_.listJson());
  }
  const std::string& id = path[1];
  if (!cloudprnt_.known(id)) {
    return fail(404, "unknown cloudprnt printer", id);
  }
  if (path.size() == 2) {
    // One URL, three verbs — the printer's console holds a single CloudPRNT address.
    if (method == "POST") {
      return cloudprnt_.poll(id, request);
    }
    if (method == "GET") {
      return cloudprnt_.fetchJob(id, request);
    }
    if (method == "DELETE") {
      return cloudprnt_.confirm(id, request);
    }
    return fail(405, "method not allowed", method);
  }
  if (path.size() == 3 && path[2] == "jobs") {
    if (method == "POST") {
      return cloudprnt_.postJob(id, request);
    }
    if (method == "GET") {
      return ok(200, cloudprnt_.jobsJson(id));
    }
    return fail(405, "method not allowed", method);
  }
  if (path.size() == 4 && path[2] == "jobs") {
    if (method != "GET") {
      return fail(405, "method not allowed", method);
    }
    return cloudprnt_.getJob(id, path[3]);
  }
  return fail(404, "not found", request.path);
}
// --- end M13b -----------------------------------------------------------------------------

}  // namespace pd::agent
