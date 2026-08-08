#include "printerdriver/capability_probe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include "platform_file.hpp"
#include "printerdriver/job_store.hpp"
#include "printerdriver/net_platform.hpp"

namespace pd {
namespace {

// Four printable bytes, outside the range the engine's MarkerAllocator hands out, so a
// probe echo can never be mistaken for a job's completion token.
constexpr char kProbeToken[] = "PRB0";

constexpr size_t kMaxUnclassifiedKept = 32;

std::string sanitize(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    const auto byte = static_cast<uint8_t>(c);
    // The record format is tab separated and line oriented; anything that could
    // break either is replaced rather than escaped.
    out += (byte < 0x20 || byte == 0x7F || c == '\t' || c == '=') ? '_' : c;
  }
  return out;
}

std::string hexOf(const std::vector<uint8_t>& bytes) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const uint8_t byte : bytes) {
    out += kDigits[byte >> 4];
    out += kDigits[byte & 0x0F];
  }
  return out;
}

std::vector<uint8_t> unhex(const std::string& text) {
  std::vector<uint8_t> out;
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < text.size(); i += 2) {
    const int high = value(text[i]);
    const int low = value(text[i + 1]);
    if (high < 0 || low < 0) {
      break;
    }
    out.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return out;
}

void appendFlag(std::string& out, const char* key, const std::optional<bool>& value) {
  if (!value.has_value()) {
    return;
  }
  out += '\t';
  out += key;
  out += *value ? "=1" : "=0";
}

void appendText(std::string& out, const char* key, const std::string& value) {
  if (value.empty()) {
    return;
  }
  out += '\t';
  out += key;
  out += '=';
  out += sanitize(value);
}

std::optional<CompletionMechanism> mechanismFromName(const std::string& name) {
  if (name == "GsParenH") return CompletionMechanism::GsParenH;
  if (name == "GsR1") return CompletionMechanism::GsR1;
  if (name == "VendorIdle") return CompletionMechanism::VendorIdle;
  if (name == "EposJobId") return CompletionMechanism::EposJobId;
  if (name == "StarCheckedBlock") return CompletionMechanism::StarCheckedBlock;
  if (name == "None") return CompletionMechanism::None;
  return std::nullopt;
}

escpos::Bytes testLine(const char* text) {
  escpos::Encoder encoder;
  encoder.line(text).feed();
  return encoder.take();
}

}  // namespace

// --- Findings -----------------------------------------------------------------------

BehaviourSignals CapabilityFindings::behaviour() const {
  BehaviourSignals signals;
  signals.gs_h_process_id = gs_h_process_id;
  signals.gs_r1 = gs_r1;
  signals.dle_eot = dle_eot;
  signals.asb = asb;
  signals.cutter_error_status = cutter_error_status;
  return signals;
}

bool CapabilityFindings::empty() const {
  return !dle_eot.has_value() && !gs_r1.has_value() && !gs_h_process_id.has_value() &&
         !asb.has_value() && !gs_i.has_value() && !cutter_error_status.has_value() &&
         !completion.has_value() && !reported.answered();
}

std::optional<CompletionMechanism> completionFrom(const CapabilityFindings& findings) {
  if (findings.gs_h_process_id.value_or(false)) {
    return CompletionMechanism::GsParenH;
  }
  if (findings.gs_r1.value_or(false)) {
    return CompletionMechanism::GsR1;
  }
  // Both mechanisms were tried and neither answered: this device has no ordered
  // fence, whatever else it may have replied to.
  if (findings.gs_h_process_id.has_value() && findings.gs_r1.has_value()) {
    return CompletionMechanism::None;
  }
  return std::nullopt;
}

CapabilityProfile promote(const CapabilityProfile& defaults,
                          const CapabilityFindings& findings) {
  CapabilityProfile profile = defaults;
  if (findings.empty()) {
    return profile;
  }
  profile.probed = true;

  if (findings.gs_h_process_id.has_value()) {
    profile.completion_caps.process_id_gs_h = *findings.gs_h_process_id;
  }
  if (findings.gs_r1.has_value()) {
    profile.completion_caps.queued_gs_r = *findings.gs_r1;
  }
  const std::optional<CompletionMechanism> mechanism =
      findings.completion.has_value() ? findings.completion : completionFrom(findings);
  if (mechanism.has_value()) {
    profile.completion = *mechanism;
  }

  if (findings.dle_eot.has_value()) {
    profile.status.dle_eot = *findings.dle_eot;
  }
  if (findings.asb.has_value()) {
    profile.status.asb = *findings.asb;
  }
  if (findings.cutter_error_status.has_value()) {
    profile.status.cutter_error = *findings.cutter_error_status;
  }

  if (findings.reported.answered()) {
    const IdentityAssessment assessment =
        identify(findings.reported, IdentityHints{}, findings.behaviour());
    profile.identity.vendor = assessment.vendor_guess;
    profile.identity.model = findings.reported.model;
    profile.identity.firmware = findings.reported.firmware;
    profile.identity.serial = findings.reported.serial;
    profile.identity.fingerprint_confidence = assessment.confidence_percent;
    profile.identity.trusted = assessment.identity_trusted;
    profile.quirks.unreliable_identity = !assessment.identity_trusted;
  }
  return profile;
}

// --- Probe --------------------------------------------------------------------------

CapabilityProbe::CapabilityProbe(ProbeOptions options) : options_(std::move(options)) {}

void CapabilityProbe::onBytes(const uint8_t* data, size_t size) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    raw_.insert(raw_.end(), data, data + size);
    if (phase_ == Phase::Escpos) {
      for (escpos::ParsedEvent& event : parser_.feed(data, size)) {
        if (event.kind == escpos::ParsedEventKind::UnknownByte &&
            unclassified_.size() < kMaxUnclassifiedKept) {
          unclassified_.push_back(event.byte);
        }
        events_.push_back(std::move(event));
      }
    }
  }
  cv_.notify_all();
}

void CapabilityProbe::beginPhase(Phase phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Expectations belong to the phase that registered them: carrying one across would
  // let the next phase's first byte answer the previous phase's question.
  parser_.reset();
  events_.clear();
  raw_.clear();
  phase_ = phase;
}

std::vector<escpos::ParsedEvent> CapabilityProbe::events() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_;
}

std::vector<uint8_t> CapabilityProbe::takeRaw() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint8_t> out;
  out.swap(raw_);
  return out;
}

bool CapabilityProbe::waitForEvents(size_t count, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this, count] { return events_.size() >= count; });
}

bool CapabilityProbe::waitForToken(const std::string& token, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, &token] {
    for (const escpos::ParsedEvent& event : events_) {
      if (event.kind == escpos::ParsedEventKind::GsHAck && event.token == token) {
        return true;
      }
    }
    return false;
  });
}

bool CapabilityProbe::waitForRawBytes(uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  // A text answer is complete at its NUL; a device that omits the terminator is
  // handled by the timeout, which is why every identification field gets its own.
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
    if (raw_.empty()) {
      return false;
    }
    if (phase_ == Phase::IdentityByte) {
      return true;
    }
    return std::find(raw_.begin(), raw_.end(), 0x00) != raw_.end();
  });
}

CapabilityFindings CapabilityProbe::run(const Writer& write) {
  CapabilityFindings findings;
  findings.endpoint = options_.endpoint;
  findings.recorded_unix_ms = unixMillis();
  if (!write) {
    return findings;
  }

  // A known starting state, and ASB off so the stream carries only answers to
  // questions this probe asked.
  {
    escpos::Encoder reset;
    reset.initialize().asb(false);
    beginPhase(Phase::Escpos);
    if (!write(reset.take())) {
      return findings;
    }
  }

  // 1. Is there a backchannel at all, and which real-time queries answer?
  const escpos::DleEotKind kinds[] = {
      escpos::DleEotKind::PrinterStatus, escpos::DleEotKind::OfflineCause,
      escpos::DleEotKind::ErrorCause, escpos::DleEotKind::PaperSensor};
  std::optional<bool>* slots[] = {&findings.dle_eot_1, &findings.dle_eot_2,
                                  &findings.dle_eot_3, &findings.dle_eot_4};
  bool any_realtime = false;
  for (size_t i = 0; i < 4; ++i) {
    beginPhase(Phase::Escpos);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      parser_.expectRealtime(kinds[i]);
    }
    if (!write(escpos::dleEot(kinds[i]))) {
      return findings;
    }
    const bool answered = waitForEvents(1, options_.status_timeout_ms);
    *slots[i] = answered;
    any_realtime = any_realtime || answered;
  }
  findings.dle_eot = any_realtime;
  // DLE EOT 3 is the query the engine reads the cutter bit from after a cut, so its
  // answer is what decides whether CutFaultFree is ever reachable here.
  findings.cutter_error_status = findings.dle_eot_3;

  // 2. Identification. Untrusted by construction — see docs/capability-profiles.md.
  bool any_identity = false;
  const escpos::PrinterInfoKind byte_kinds[] = {escpos::PrinterInfoKind::ModelId,
                                                escpos::PrinterInfoKind::TypeId,
                                                escpos::PrinterInfoKind::RomVersionId};
  std::optional<uint8_t>* byte_slots[] = {&findings.reported.model_id,
                                          &findings.reported.type_id,
                                          &findings.reported.rom_version};
  for (size_t i = 0; i < 3; ++i) {
    beginPhase(Phase::IdentityByte);
    if (!write(escpos::gsIdentity(byte_kinds[i]))) {
      return findings;
    }
    waitForRawBytes(options_.identity_timeout_ms);
    const std::optional<uint8_t> answer = parseGsIByte(takeRaw());
    *byte_slots[i] = answer;
    any_identity = any_identity || answer.has_value();
  }

  const escpos::PrinterInfoKind text_kinds[] = {escpos::PrinterInfoKind::MakerName,
                                                escpos::PrinterInfoKind::ModelName,
                                                escpos::PrinterInfoKind::FirmwareVersion,
                                                escpos::PrinterInfoKind::SerialNumber};
  std::string* text_slots[] = {&findings.reported.manufacturer, &findings.reported.model,
                               &findings.reported.firmware, &findings.reported.serial};
  for (size_t i = 0; i < 4; ++i) {
    beginPhase(Phase::IdentityString);
    if (!write(escpos::gsIdentity(text_kinds[i]))) {
      return findings;
    }
    waitForRawBytes(options_.identity_timeout_ms);
    const std::optional<std::string> answer = parseGsIString(takeRaw());
    if (answer.has_value()) {
      *text_slots[i] = *answer;
      any_identity = true;
    }
  }
  findings.gs_i = any_identity;

  // 3. GS ( H fn 48 behind a short line: the per-receipt correlation token, and the
  // only mechanism that lets a completion be attributed to one job.
  beginPhase(Phase::Escpos);
  {
    escpos::Bytes probe;
    if (options_.print_test_lines) {
      const escpos::Bytes line = testLine("pd capability probe: GS ( H");
      probe.insert(probe.end(), line.begin(), line.end());
    }
    const escpos::Bytes marker = escpos::processIdMarker(kProbeToken);
    probe.insert(probe.end(), marker.begin(), marker.end());
    if (!write(probe)) {
      return findings;
    }
  }
  findings.gs_h_process_id = waitForToken(kProbeToken, options_.completion_timeout_ms);

  // 4. GS r 1, the documented fallback fence.
  beginPhase(Phase::Escpos);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.expectQueued();
  }
  {
    escpos::Bytes probe;
    if (options_.print_test_lines) {
      const escpos::Bytes line = testLine("pd capability probe: GS r 1");
      probe.insert(probe.end(), line.begin(), line.end());
    }
    const escpos::Bytes fence = escpos::gsPaperStatus();
    probe.insert(probe.end(), fence.begin(), fence.end());
    if (!write(probe)) {
      return findings;
    }
  }
  findings.gs_r1 = waitForEvents(1, options_.completion_timeout_ms);

  // 5. ASB on, then off again. Enabling it makes the device volunteer status; leaving
  // it on would inject frames into every later job's stream.
  beginPhase(Phase::Escpos);
  if (!write(escpos::asbEnable())) {
    return findings;
  }
  waitForEvents(1, options_.status_timeout_ms);
  bool asb_seen = false;
  for (const escpos::ParsedEvent& event : events()) {
    asb_seen = asb_seen || event.kind == escpos::ParsedEventKind::AsbStatus;
  }
  findings.asb = asb_seen;
  write(escpos::asbDisable());

  findings.completion = completionFrom(findings);
  findings.key = identityKey(findings.reported, options_.endpoint);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    findings.unclassified = unclassified_;
  }
  return findings;
}

// --- Persistence --------------------------------------------------------------------

std::string serializeFindings(const CapabilityFindings& findings) {
  std::string out = "key=" + sanitize(findings.key);
  appendText(out, "endpoint", findings.endpoint);
  out += "\tat=" + std::to_string(findings.recorded_unix_ms);
  appendFlag(out, "dle_eot", findings.dle_eot);
  appendFlag(out, "dle_eot_1", findings.dle_eot_1);
  appendFlag(out, "dle_eot_2", findings.dle_eot_2);
  appendFlag(out, "dle_eot_3", findings.dle_eot_3);
  appendFlag(out, "dle_eot_4", findings.dle_eot_4);
  appendFlag(out, "gs_r1", findings.gs_r1);
  appendFlag(out, "gs_h", findings.gs_h_process_id);
  appendFlag(out, "asb", findings.asb);
  appendFlag(out, "gs_i", findings.gs_i);
  appendFlag(out, "cutter_status", findings.cutter_error_status);
  if (findings.completion.has_value()) {
    out += "\tcompletion=";
    out += to_string(*findings.completion);
  }
  appendText(out, "maker", findings.reported.manufacturer);
  appendText(out, "model", findings.reported.model);
  appendText(out, "firmware", findings.reported.firmware);
  appendText(out, "serial", findings.reported.serial);
  if (findings.reported.model_id.has_value()) {
    out += "\tmodel_id=" + std::to_string(*findings.reported.model_id);
  }
  if (findings.reported.type_id.has_value()) {
    out += "\ttype_id=" + std::to_string(*findings.reported.type_id);
  }
  if (findings.reported.rom_version.has_value()) {
    out += "\trom_id=" + std::to_string(*findings.reported.rom_version);
  }
  if (!findings.unclassified.empty()) {
    out += "\tunclassified=" + hexOf(findings.unclassified);
  }
  return out;
}

std::optional<CapabilityFindings> parseFindings(const std::string& line) {
  if (line.empty()) {
    return std::nullopt;
  }
  CapabilityFindings findings;
  bool has_key = false;
  std::istringstream stream(line);
  std::string field;
  while (std::getline(stream, field, '\t')) {
    const size_t equals = field.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string name = field.substr(0, equals);
    const std::string value = field.substr(equals + 1);
    const auto flag = [&value]() -> std::optional<bool> { return value == "1"; };
    const auto number = [&value]() -> uint64_t {
      return std::strtoull(value.c_str(), nullptr, 10);
    };
    if (name == "key") {
      findings.key = value;
      has_key = true;
    } else if (name == "endpoint") {
      findings.endpoint = value;
    } else if (name == "at") {
      findings.recorded_unix_ms = number();
    } else if (name == "dle_eot") {
      findings.dle_eot = flag();
    } else if (name == "dle_eot_1") {
      findings.dle_eot_1 = flag();
    } else if (name == "dle_eot_2") {
      findings.dle_eot_2 = flag();
    } else if (name == "dle_eot_3") {
      findings.dle_eot_3 = flag();
    } else if (name == "dle_eot_4") {
      findings.dle_eot_4 = flag();
    } else if (name == "gs_r1") {
      findings.gs_r1 = flag();
    } else if (name == "gs_h") {
      findings.gs_h_process_id = flag();
    } else if (name == "asb") {
      findings.asb = flag();
    } else if (name == "gs_i") {
      findings.gs_i = flag();
    } else if (name == "cutter_status") {
      findings.cutter_error_status = flag();
    } else if (name == "completion") {
      findings.completion = mechanismFromName(value);
    } else if (name == "maker") {
      findings.reported.manufacturer = value;
    } else if (name == "model") {
      findings.reported.model = value;
    } else if (name == "firmware") {
      findings.reported.firmware = value;
    } else if (name == "serial") {
      findings.reported.serial = value;
    } else if (name == "model_id") {
      findings.reported.model_id = static_cast<uint8_t>(number());
    } else if (name == "type_id") {
      findings.reported.type_id = static_cast<uint8_t>(number());
    } else if (name == "rom_id") {
      findings.reported.rom_version = static_cast<uint8_t>(number());
    } else if (name == "unclassified") {
      findings.unclassified = unhex(value);
    }
  }
  if (!has_key) {
    return std::nullopt;
  }
  return findings;
}

FindingsStore::FindingsStore(std::string directory, std::string file_name) {
  if (directory.empty()) {
    return;
  }
  if (!platform_file::createDirectory(directory, nullptr)) {
    // A findings cache that cannot be written is a performance problem, never a
    // correctness one: the probe simply runs again next boot.
    return;
  }
  if (platform_file::isSeparator(directory.back())) {
    directory.pop_back();
  }
  path_ = directory + "/" + file_name;
  load();
}

void FindingsStore::load() {
  std::ifstream input(path_);
  if (!input.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (std::optional<CapabilityFindings> record = parseFindings(line)) {
      records_.push_back(std::move(*record));
    }
  }
}

void FindingsStore::flush() const {
  if (path_.empty()) {
    return;
  }
  const std::string temp = path_ + ".tmp";
  const NativeFile file = platform_file::openTruncate(temp, nullptr);
  if (!nativeFileValid(file)) {
    return;
  }
  std::string blob;
  for (const CapabilityFindings& record : records_) {
    blob += serializeFindings(record);
    blob += '\n';
  }
  const bool ok = platform_file::writeAll(file, blob.data(), blob.size(), nullptr);
  if (ok) {
    platform_file::sync(file, nullptr);
  }
  platform_file::closeFile(file);
  if (ok) {
    platform_file::replaceFile(temp, path_, nullptr);
  } else {
    platform_file::removeFile(temp);
  }
}

std::optional<CapabilityFindings> FindingsStore::find(const std::string& key) const {
  if (key.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const CapabilityFindings& record : records_) {
    if (record.key == key) {
      return record;
    }
  }
  return std::nullopt;
}

std::optional<CapabilityFindings> FindingsStore::findByEndpoint(
    const std::string& endpoint) const {
  if (endpoint.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const CapabilityFindings& record : records_) {
    if (record.endpoint == endpoint) {
      return record;
    }
  }
  return std::nullopt;
}

void FindingsStore::save(const CapabilityFindings& findings) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool replaced = false;
  for (CapabilityFindings& record : records_) {
    if (record.key == findings.key) {
      record = findings;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    records_.push_back(findings);
  }
  flush();
}

size_t FindingsStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

// --- Port reachability ---------------------------------------------------------------

bool tcpPortOpen(const std::string& host, uint16_t port, uint32_t timeout_ms) {
  // Winsock refuses getaddrinfo too, not just socket(), until WSAStartup has run. A
  // no-op on POSIX.
  if (!net::startup()) {
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0) {
    return false;
  }
  bool open = false;
  for (addrinfo* it = resolved; it != nullptr && !open; it = it->ai_next) {
    const net::Socket socket = net::create(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (!net::valid(socket)) {
      continue;
    }
    net::setNonBlocking(socket);
    if (::connect(socket, it->ai_addr, static_cast<net::SockLen>(it->ai_addrlen)) == 0) {
      open = true;
    } else if (net::inProgress(net::lastError())) {
      net::PollFd waiter;
      waiter.socket = socket;
      waiter.events = net::kPollOut;
      if (net::poll(&waiter, 1, static_cast<int>(timeout_ms)) > 0) {
        int error = 0;
        if (net::pendingError(socket, &error) && error == 0) {
          open = true;
        }
      }
    }
    net::closeSocket(socket);
  }
  ::freeaddrinfo(resolved);
  return open;
}

}  // namespace pd
