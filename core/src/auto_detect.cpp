#include "printerdriver/self_test.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/transport.hpp"

// M15 — auto-detection, and the report structure the self-test shares with it
// (docs/api.md §15).
//
// Composition only. Every question this file asks has been asked before by
// `pdctl probe`; the contribution here is that it asks them for a whole subnet at once,
// respects the findings cache, and — the load-bearing constraint — NEVER PRINTS.
//
// -- The printing rule, spelled out ---------------------------------------------------
//
// A port-9100 device prints what it receives (discovery.hpp). The sweep therefore writes
// exactly `10 04 01` and nothing else, and the per-candidate probe below runs with
// ProbeOptions::print_test_lines = false, which removes the two short test lines the full
// probe puts ahead of its fences. What remains — ESC @, GS a on/off, DLE EOT 1-4, GS I,
// one GS ( H marker, one GS r 1 — is entirely non-printing.
//
// The price is stated on every report this file produces: an ordered fence asked out of
// an EMPTY buffer proves that the firmware implements the command, not that its answer
// waits for paper to move (docs/techspec.md §3.2). So the flag is promoted and its
// provenance is not. Full promotion still needs `pdctl probe`, which prints, or a real
// job.

namespace pd {
namespace {

// Font A is 12 dots wide at 203 dpi, which is what makes an 80 mm / 576 dot printer 48
// columns (docs/api.md §13). The renderer owns the same constant; this is the core-side
// copy for a summary built without one.
constexpr uint32_t kFontADots = 12;

bool isGenericProfileName(const std::string& name) {
  return name.empty() || name == "generic_80" || name == "generic_58" ||
         name == "generic_unknown" || name == "generic-escpos";
}

bool answeredAnything(const CapabilityFindings& findings) {
  return findings.reported.answered() || findings.dle_eot.value_or(false) ||
         findings.gs_r1.value_or(false) || findings.gs_h_process_id.value_or(false) ||
         findings.asb.value_or(false);
}

struct Candidate {
  std::string host;
  uint16_t port = 9100;
  bool swept = false;  // the sweep already established the facts below
  bool port_open = false;
  std::vector<uint8_t> dle_eot_response;
};

// "host", "host:port", "[v6]:port" is out of scope — discovery.hpp is IPv4 only.
Candidate parseEndpoint(const std::string& text, uint16_t default_port) {
  Candidate out;
  out.port = default_port;
  const size_t colon = text.rfind(':');
  if (colon == std::string::npos) {
    out.host = text;
    return out;
  }
  const std::string port_text = text.substr(colon + 1);
  bool digits = !port_text.empty();
  for (const char c : port_text) {
    digits = digits && c >= '0' && c <= '9';
  }
  if (!digits) {
    out.host = text;
    return out;
  }
  const long value = std::strtol(port_text.c_str(), nullptr, 10);
  out.host = text.substr(0, colon);
  if (value > 0 && value <= 65535) {
    out.port = static_cast<uint16_t>(value);
  }
  return out;
}

}  // namespace

// --- Shared report structure ---------------------------------------------------------

const char kSdkVersion[] = "0.1.0";

// docs/api.md §15's own sample line, verbatim: Czech, Hungarian, Polish.
const char kCharsetSample[] = "příliš žluťoučký kůň / árvíztűrő / zażółć";

const char* to_string(ProfileSelection value) noexcept {
  switch (value) {
    case ProfileSelection::Documented: return "Documented";
    case ProfileSelection::Probed: return "Probed";
    case ProfileSelection::Default: return "Default";
  }
  return "Default";
}

const char* to_string(DetectionStatus value) noexcept {
  switch (value) {
    case DetectionStatus::Answered: return "Answered";
    case DetectionStatus::Silent: return "Silent";
    case DetectionStatus::Unverified: return "Unverified";
    case DetectionStatus::Unreachable: return "Unreachable";
  }
  return "Unreachable";
}

std::string DetectionSummary::provenanceSummary() const {
  std::string out = std::string(method) + " " + to_string(completion_provenance);
  out += " - profile ";
  out += to_string(selection);
  out += " - identity ";
  out += identity.trusted ? "trusted" : "untrusted";
  out += " (" + std::to_string(static_cast<int>(identity.confidence_percent)) + "%)";
  return out;
}

std::string SelfTestResult::ticketText() const {
  std::string out;
  for (const std::string& line : ticket_lines) {
    out += line;
    out += '\n';
  }
  return out;
}

// --- Summary construction ------------------------------------------------------------

namespace detail {

// Which provenance column governs the mechanism actually in force. Reading the wrong
// column is how "GS ( H is documented for Epson" becomes a claim about a clone.
Provenance completionProvenanceOf(const CapabilityProfile& profile) noexcept {
  switch (profile.completion) {
    case CompletionMechanism::GsParenH:
      return profile.completion_caps.process_id_gs_h_provenance;
    case CompletionMechanism::GsR1:
      return profile.completion_caps.queued_gs_r_provenance;
    case CompletionMechanism::VendorIdle:
      return profile.completion_caps.vendor_idle_provenance;
    case CompletionMechanism::EposJobId:
      return profile.completion_caps.epos_job_id_provenance;
    case CompletionMechanism::StarEtb:
      return profile.star.etb_provenance;
    case CompletionMechanism::StarEscGsEtx:
      return profile.star.esc_gs_etx_provenance;
    case CompletionMechanism::StarCheckedBlock:
    case CompletionMechanism::None:
      break;
  }
  return Provenance::Unverified;
}

ProfileSelection selectionFor(const CapabilityProfile& profile) noexcept {
  if (profile.probed) {
    return ProfileSelection::Probed;
  }
  return isGenericProfileName(profile.name) ? ProfileSelection::Default
                                            : ProfileSelection::Documented;
}

DetectionSummary summarize(const std::string& endpoint, const CapabilityProfile& profile,
                           const IdentityAssessment& assessment) {
  DetectionSummary summary;
  summary.endpoint = endpoint;

  summary.identity.vendor = assessment.vendor_guess;
  summary.identity.model = assessment.reported_model.empty() ? profile.identity.model
                                                             : assessment.reported_model;
  summary.identity.firmware = assessment.firmware;
  summary.identity.serial = assessment.serial;
  summary.identity.trusted = assessment.identity_trusted;
  summary.identity.confidence_percent = assessment.confidence_percent;
  summary.identity.impersonation_suspected = assessment.impersonation_suspected;
  summary.identity.signals = assessment.signals;

  summary.profile_id = profile.name;
  summary.selection = selectionFor(profile);

  summary.nominal_paper_mm = profile.media.nominal_roll_width_mm;
  summary.printable_width_dots = profile.media.printable_width_dots;
  summary.chars_per_line = profile.media.printable_width_dots / kFontADots;
  summary.dpi = profile.media.dpi;

  const JobEvidence evidence = profile.evidence();
  summary.completion = profile.completion;
  summary.grade_ceiling = evidence.grade;
  summary.authority = evidence.authority;
  summary.method = evidence.method;
  summary.completion_provenance = completionProvenanceOf(profile);

  summary.drawer_present = profile.drawer.present;
  summary.drawer_kickable = profile.drawer.kickable();
  summary.drawer_standard = profile.drawer.electrical.standard;
  summary.drawer_voltage = profile.drawer.electrical.voltage;
  summary.drawer_electrical_provenance = profile.drawer.evidence.electrical;
  summary.drawer_commands_provenance = profile.drawer.evidence.commands;
  return summary;
}

}  // namespace detail

// --- autoDetect ----------------------------------------------------------------------

namespace {

// One candidate, start to finish. Runs on a worker thread and touches nothing shared
// except the findings store, which has its own lock.
DetectedPrinter classify(const Candidate& candidate, const AutoDetectOptions& options,
                         FindingsStore* cache) {
  DetectedPrinter out;
  out.host = candidate.host;
  out.port = candidate.port;
  out.endpoint = candidate.host + ":" + std::to_string(candidate.port);

  DiscoveryOptions sweep;
  sweep.port = candidate.port;
  sweep.connect_timeout_ms = options.connect_timeout_ms;
  sweep.response_timeout_ms = options.response_timeout_ms;

  if (candidate.swept) {
    out.port_open = candidate.port_open;
    out.dle_eot_response = candidate.dle_eot_response;
  } else {
    // Named by the caller rather than found by a sweep: ask the same three bytes the
    // sweep would have, so every candidate is classified from the same evidence.
    const DiscoveredDevice device = probeHost(candidate.host, sweep);
    out.port_open = device.port9100_open;
    out.dle_eot_response = device.dle_eot_response;
  }

  if (!out.port_open) {
    out.status = DetectionStatus::Unreachable;
    out.summary.endpoint = out.endpoint;
    out.summary.degradations.push_back(
        "port " + std::to_string(candidate.port) + " refused the connection or timed out");
    return out;
  }

  std::optional<CapabilityFindings> stored;
  if (cache != nullptr) {
    stored = cache->findByEndpoint(out.endpoint);
  }

  if (!stored.has_value() && !options.probe_unknown) {
    // Reachable, deliberately untouched. This is a different answer from "no
    // capabilities" and is never rendered as one: nobody asked.
    out.status = DetectionStatus::Unverified;
    const CapabilityProfile profile = devices::generic_unknown();
    out.summary = detail::summarize(out.endpoint, profile, IdentityAssessment{});
    out.summary.degradations.push_back(
        "not interrogated: probeUnknown is off and nothing is cached for this device");
    return out;
  }

  CapabilityFindings findings;
  if (stored.has_value()) {
    findings = *stored;
    out.from_cache = true;
  } else {
    ProbeOptions probe_options;
    probe_options.endpoint = out.endpoint;
    // The whole safety property of this call, in one assignment.
    probe_options.print_test_lines = options.allow_printing_probe;
    probe_options.status_timeout_ms = options.status_timeout_ms;
    probe_options.identity_timeout_ms = options.identity_timeout_ms;
    probe_options.completion_timeout_ms = options.completion_timeout_ms;

    TcpConfig tcp;
    tcp.host = candidate.host;
    tcp.port = candidate.port;
    tcp.connect_timeout_ms = options.connect_timeout_ms;
    TcpTransport transport(tcp);
    CapabilityProbe probe(probe_options);
    transport.onBytes(
        [&probe](const uint8_t* data, size_t size) { probe.onBytes(data, size); });
    transport.onDisconnected([](TransportError, const std::string&) {});
    if (!transport.connect().ok) {
      out.status = DetectionStatus::Unreachable;
      out.summary.endpoint = out.endpoint;
      out.summary.degradations.push_back("the port answered the sweep and then refused a "
                                         "second connection");
      return out;
    }
    findings = probe.run([&transport](const escpos::Bytes& bytes) {
      return transport.write(bytes).ok;
    });
    transport.close();
    findings.endpoint = out.endpoint;
    if (cache != nullptr && cache->persistent() && !findings.empty()) {
      cache->save(findings);
    }
  }

  const IdentityAssessment assessment =
      identify(findings.reported, IdentityHints{}, findings.behaviour());
  const CapabilityProfile defaults = devices::byName(assessment.profile_guess);
  CapabilityProfile profile = promote(defaults, findings);

  const bool printless = !out.from_cache && !options.allow_printing_probe;
  if (printless) {
    // The flag stays promoted — the device really did answer — but the provenance goes
    // back to what the database shipped. An echo out of an empty buffer is evidence that
    // the command exists, not that it fences a print.
    profile.completion_caps.process_id_gs_h_provenance =
        defaults.completion_caps.process_id_gs_h_provenance;
    profile.completion_caps.queued_gs_r_provenance =
        defaults.completion_caps.queued_gs_r_provenance;
  }

  out.summary = detail::summarize(out.endpoint, profile, assessment);
  out.summary.identity_fresh = !out.from_cache;
  out.status = answeredAnything(findings) ? DetectionStatus::Answered
                                          : DetectionStatus::Silent;

  if (printless && profile.completion != CompletionMechanism::None) {
    out.summary.degradations.push_back(
        "completion asked out of an empty buffer: the printless probe proves the command "
        "exists, not that its answer waits for paper - provenance not promoted");
  }
  if (out.from_cache) {
    out.summary.degradations.push_back(
        "classified from stored findings; the device was not interrogated again");
  }
  if (out.status == DetectionStatus::Silent) {
    out.summary.degradations.push_back(
        "the port accepted the connection and answered nothing: an interface that does "
        "not forward status bytes, so every claim here is the profile default");
  }
  if (!assessment.identity_trusted && findings.reported.answered()) {
    out.summary.degradations.push_back(
        "identity NOT trusted: GS I is a string the firmware chooses and at least one "
        "family ships answering as somebody else's model");
  }
  return out;
}

}  // namespace

std::vector<DetectedPrinter> PrinterDriver::autoDetect(
    const AutoDetectOptions& options, const AutoDetectProgressCallback& on_candidate) {
  std::vector<Candidate> candidates;

  if (!options.endpoints.empty()) {
    candidates.reserve(options.endpoints.size());
    for (const std::string& text : options.endpoints) {
      candidates.push_back(parseEndpoint(text, options.port));
    }
  } else {
    DiscoveryOptions sweep;
    sweep.port = options.port;
    sweep.concurrency = options.concurrency == 0 ? 1 : options.concurrency;
    sweep.connect_timeout_ms = options.connect_timeout_ms;
    sweep.response_timeout_ms = options.response_timeout_ms;
    // The sweep's only write is DLE EOT 1. Leaving it on costs nothing and is what
    // separates "something ESC/POS-shaped is listening" from "a port is open".
    sweep.probe_backchannel = true;

    std::vector<DiscoveredDevice> found;
    if (options.subnet_cidr.empty()) {
      found = discover(sweep);
    } else {
      found = discover(options.subnet_cidr, sweep);
    }
    // Only open ports become candidates on a sweep: a /24 reports 254 closed addresses
    // and none of them is a printer. A caller who wants a closed address on the report
    // names it in `endpoints`, where the refusal is itself the answer.
    candidates.reserve(found.size());
    for (const DiscoveredDevice& device : found) {
      Candidate candidate;
      candidate.host = device.ip;
      candidate.port = device.port;
      candidate.swept = true;
      candidate.port_open = device.port9100_open;
      candidate.dle_eot_response = device.dle_eot_response;
      candidates.push_back(std::move(candidate));
    }
  }

  const uint64_t total = candidates.size();
  std::vector<DetectedPrinter> results(candidates.size());
  if (candidates.empty()) {
    return results;
  }

  FindingsStore* cache = capabilities_.get();
  std::atomic<size_t> next{0};
  std::atomic<uint64_t> completed{0};
  std::mutex callback_mutex;

  const size_t workers = std::min<size_t>(
      candidates.size(), options.concurrency == 0 ? 1 : options.concurrency);
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (size_t i = 0; i < workers; ++i) {
    threads.emplace_back([&] {
      for (;;) {
        const size_t index = next.fetch_add(1);
        if (index >= candidates.size()) {
          return;
        }
        results[index] = classify(candidates[index], options, cache);
        const uint64_t done = completed.fetch_add(1) + 1;
        if (on_candidate) {
          // Serialised, because a UI callback that has to be thread-safe as well as
          // non-blocking is a callback nobody gets right.
          std::lock_guard<std::mutex> lock(callback_mutex);
          on_candidate(results[index], done, total);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return results;
}

}  // namespace pd
