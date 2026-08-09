#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/capability_profile.hpp"
#include "printerdriver/identity.hpp"
// M16 — registered probe steps (docs/api.md §16) extend the fingerprint below.
#include "printerdriver/registrations.hpp"
#include "printerdriver/response_parser.hpp"

// Non-destructive capability interrogation and the promotion of a profile from its
// shipped defaults to what the device actually does
// (docs/capability-profiles.md §8, docs/device-database.md "Probe expansion").
//
// What the automatic probe sends: DLE EOT 1-4, GS I, one GS ( H marker behind a short
// test line, GS r 1, and ASB on/off.
//
// What it never sends, at any confidence level: DLE ENQ (resumes a job or discards a
// partly printed one), DLE DC4 fn 2 (powers the device off), DLE DC4 fn 8 (clears the
// buffers), and any cut. On a kitchen printer each of those is either a duplicate
// ticket or a lost one, so they exist only behind an explicit operator action.

namespace pd {

struct ProbeOptions {
  uint32_t status_timeout_ms = 1500;
  // Per GS I field, so a printer that ignores one identification type costs one
  // timeout rather than stalling the whole identification phase.
  uint32_t identity_timeout_ms = 1200;
  uint32_t completion_timeout_ms = 8000;
  // Two short lines of text. The ordered fences only mean anything when there is
  // print data ahead of them for the device to finish first.
  bool print_test_lines = true;
  std::string endpoint;
  IdentityHints hints;
  // M16 (docs/api.md §16). House-specific probe steps registered on the driver, run after
  // the built-in phases. Each is non-printing (enforced at registration), so they hold on
  // the printless autoDetect path as well as the full probe.
  std::vector<ProbeStep> custom_steps;
};

// M16. The outcome of one registered probe step, attributed to its id.
struct CustomProbeResult {
  std::string id;
  ProbeFinding finding;
};

// Everything the probe established first-hand. Every field is optional because "not
// established" and "not supported" are different answers, and only the probe knows
// which one it got.
struct CapabilityFindings {
  std::string key;       // identityKey(): model+firmware+serial, else the endpoint
  std::string endpoint;  // where this was observed
  uint64_t recorded_unix_ms = 0;

  std::optional<bool> dle_eot;  // any of 1-4 answered
  std::optional<bool> dle_eot_1;
  std::optional<bool> dle_eot_2;
  std::optional<bool> dle_eot_3;
  std::optional<bool> dle_eot_4;
  std::optional<bool> gs_r1;
  std::optional<bool> gs_h_process_id;
  std::optional<bool> asb;
  std::optional<bool> gs_i;
  std::optional<bool> cutter_error_status;
  std::optional<CompletionMechanism> completion;

  GsIStrings reported;
  // Bytes the response parser could not classify. On unfamiliar hardware this is the
  // interesting part of the probe, so it is kept rather than discarded.
  std::vector<uint8_t> unclassified;
  // M16. Results of the registered custom probe steps, attributed by id. Not persisted:
  // the steps themselves are process-local, so their findings are recomputed each run.
  std::vector<CustomProbeResult> custom_probe;

  BehaviourSignals behaviour() const;
  bool empty() const;
};

// Findings win over profile defaults, in both directions: an established capability
// promotes a generic profile, and an established absence demotes a shipped one. An
// unset finding leaves the default alone — silence is never evidence.
CapabilityProfile promote(const CapabilityProfile& defaults,
                          const CapabilityFindings& findings);

// The strongest ordered fence the findings support, or nullopt when the probe could
// not establish one either way.
std::optional<CompletionMechanism> completionFrom(const CapabilityFindings& findings);

// Drives the interrogation. The caller owns the connection: it feeds received bytes to
// onBytes() and hands run() a writer. Nothing here opens a socket, so the same probe
// runs over TCP, serial or a test double.
class CapabilityProbe {
 public:
  using Writer = std::function<bool(const escpos::Bytes&)>;

  explicit CapabilityProbe(ProbeOptions options = {});

  // Safe to call from the transport's reader thread.
  void onBytes(const uint8_t* data, size_t size);

  CapabilityFindings run(const Writer& write);

 private:
  enum class Phase { Escpos, IdentityString, IdentityByte };

  void beginPhase(Phase phase);
  bool waitForEvents(size_t count, uint32_t timeout_ms);
  bool waitForToken(const std::string& token, uint32_t timeout_ms);
  bool waitForRawBytes(uint32_t timeout_ms);
  std::vector<uint8_t> takeRaw();
  std::vector<escpos::ParsedEvent> events() const;

  ProbeOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  Phase phase_ = Phase::Escpos;
  escpos::ResponseParser parser_;
  std::vector<escpos::ParsedEvent> events_;
  std::vector<uint8_t> raw_;
  std::vector<uint8_t> unclassified_;
};

// --- Persistence --------------------------------------------------------------------
// Probe results are stored so a fleet does not re-interrogate every printer on every
// boot (docs/capability-profiles.md: "persisted keyed by model + serial + firmware").

std::string serializeFindings(const CapabilityFindings& findings);
std::optional<CapabilityFindings> parseFindings(const std::string& line);

class FindingsStore {
 public:
  // An empty directory means in-memory only, matching StorageConfig.
  explicit FindingsStore(std::string directory,
                         std::string file_name = "capabilities.db");

  std::optional<CapabilityFindings> find(const std::string& key) const;
  std::optional<CapabilityFindings> findByEndpoint(const std::string& endpoint) const;
  // Replaces any record with the same key and rewrites the file.
  void save(const CapabilityFindings& findings);

  size_t size() const;
  const std::string& path() const noexcept { return path_; }
  bool persistent() const noexcept { return !path_.empty(); }

 private:
  void load();
  void flush() const;

  mutable std::mutex mutex_;
  std::string path_;
  std::vector<CapabilityFindings> records_;
};

// Connect-and-drop reachability check for the discovery report's interface section.
bool tcpPortOpen(const std::string& host, uint16_t port, uint32_t timeout_ms);

}  // namespace pd
