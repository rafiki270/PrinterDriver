#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/capability_probe.hpp"
#include "printerdriver/capability_profile.hpp"
#include "printerdriver/cash_drawer.hpp"
#include "printerdriver/discovery.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/identity.hpp"
#include "printerdriver/types.hpp"

// M15 — self-test and auto-detection (docs/api.md §15).
//
// Two composition APIs and no new protocol. Everything below is assembled out of
// machinery that already exists and is already tested: discovery (discovery.hpp),
// multi-signal identification (identity.hpp), the non-destructive capability probe
// (capability_probe.hpp), the receipt DSL renderer, and the ordinary fenced job engine
// with its GS ( H verification trailer (driver.hpp, docs/api.md §14). Not one byte of
// new wire format is defined here.
//
// -- The two calls -------------------------------------------------------------------
//
//   PrinterDriver::autoDetect()  reads. It sweeps, identifies and classifies, and
//                                NOTHING PRINTS AND NOTHING FIRES. See AutoDetectOptions
//                                for exactly which probe subset that restricts it to and
//                                what it therefore may not claim.
//
//   Printer::selfTest()          writes exactly one ticket, through the ordinary engine,
//                                under an idempotency key and a real completion fence.
//                                The paper is the detection report and the returned
//                                JobResult is the proof: a Done at grade A *is* the
//                                statement that this stack works end to end on this unit.
//
// -- Where selfTest is compiled ------------------------------------------------------
//
// Printer::selfTest is declared in driver.hpp with the rest of Printer, and DEFINED one
// layer up, in the receipt-DSL library (dsl/src/self_test.cpp). The ticket is a DSL
// document — that is what gives it a laid-out charset line, a real Code 128 symbol and,
// above all, a RenderReport whose declared degradations are printed ON the paper instead
// of being swallowed. The DSL depends on the core, so the core cannot depend on the DSL;
// the member therefore lives in the layer that has both. A program that links
// printerdriver_core alone and calls selfTest gets a link error naming it, which is the
// honest failure for a call whose whole job is to render a document.
//
// PrinterDriver::autoDetect has no such constraint and is compiled into the core
// (core/src/auto_detect.cpp).

namespace pd {

class PrintJob;

// The SDK version the self-test ticket prints. Mirrors project(VERSION) in
// CMakeLists.txt — there is nowhere else in the tree that carries it, and a diagnostic
// ticket that cannot say which build produced it is worth much less six months later.
extern const char kSdkVersion[];

// The charset sample of docs/api.md §15, verbatim: Czech, Hungarian and Polish in one
// line. Every letter in it is in PC852 (Latin-2), which is why the ticket selects that
// code page rather than the profile's own — a self-test that printed question marks
// would be testing the wrong thing.
extern const char kCharsetSample[];

// How the capability profile in force was arrived at (docs/api.md §15 "PROFILE ...
// selected by DOCUMENTED|PROBED|DEFAULT"). Deliberately not Provenance: that answers
// "where does the claim about this *capability* come from", and this answers "where did
// this *profile* come from". A profile can be selected by documentation and then have a
// single capability promoted by a probe.
enum class ProfileSelection {
  // A device-database entry matched what the device reported about itself
  // (identity.hpp profileForModel). Documentation picked the profile.
  Documented,
  // A capability probe's first-hand findings promoted whatever was selected, so the
  // profile in force is no longer the shipped default (CapabilityProfile::probed).
  Probed,
  // Neither. The caller's or the driver's default is the whole truth, which per
  // docs/capability-profiles.md §8 means UNKNOWN DEVICE rather than ordinary device.
  Default,
};

// What autoDetect was able to establish about one address. Four answers, because
// collapsing any two of them loses the distinction an installer needs.
enum class DetectionStatus {
  // The port answered on the backchannel: identification, fences, or both.
  Answered,
  // The port accepted the connection and said nothing at all. A real finding, not a
  // failure — it is the LAN module that does not forward status bytes
  // (docs/techspec.md §4), and it means every claim about this device stays at the
  // profile's shipped default.
  Silent,
  // Reachable and deliberately not interrogated: probeUnknown was false and nothing is
  // cached for it. Never rendered as "no capabilities" — nobody asked.
  Unverified,
  // The connection was refused, timed out, or the port is closed.
  Unreachable,
};

// Declaration order, for wrapper generators and the bridge tests that enumerate members
// without a hand-maintained list (same contract as types.hpp's kAll* arrays).
constexpr std::array<ProfileSelection, 3> kAllProfileSelections{
    ProfileSelection::Documented,
    ProfileSelection::Probed,
    ProfileSelection::Default,
};

constexpr std::array<DetectionStatus, 4> kAllDetectionStatuses{
    DetectionStatus::Answered,
    DetectionStatus::Silent,
    DetectionStatus::Unverified,
    DetectionStatus::Unreachable,
};

const char* to_string(ProfileSelection) noexcept;
const char* to_string(DetectionStatus) noexcept;

// What the device said about itself plus what that is worth. The whole struct is
// evidence, never truth: `trusted` is false until a signal independent of GS I agrees
// with GS I, because Rongta's own manual documents its printers answering "EPOSN" /
// "TM-T88V" (docs/capability-profiles.md).
struct DetectedIdentity {
  std::string vendor = "Unknown";
  std::string model;
  std::string firmware;
  std::string serial;
  bool trusted = false;
  uint8_t confidence_percent = 0;
  bool impersonation_suspected = false;
  // identify()'s own reasons, in report order — what `pdctl identify` prints and what an
  // operator needs when the guess is wrong.
  std::vector<std::string> signals;
};

// Everything the self-test established about the unit it printed on, and the same
// structure autoDetect fills in per candidate. Plain data: serializable, and carried
// across the C ABI field by field.
struct DetectionSummary {
  std::string endpoint;  // printer id or "host:port"

  DetectedIdentity identity;
  // Whether the identification above came from this run rather than from the findings
  // store. A cached identity is not stale by definition — probe results are keyed by
  // model+firmware+serial precisely so they survive a DHCP lease — but which one it is
  // belongs on the report.
  bool identity_fresh = false;

  std::string profile_id;
  ProfileSelection selection = ProfileSelection::Default;

  // Media (docs/device-database.md: roll width and raster width are separate facts).
  uint16_t nominal_paper_mm = 0;
  uint32_t printable_width_dots = 0;
  uint32_t chars_per_line = 0;  // font A at this width
  uint16_t dpi = 0;

  // Completion: the mechanism, the best grade a job on it can ever claim, who is
  // making that claim, and what the claim rests on.
  CompletionMechanism completion = CompletionMechanism::None;
  ConfidenceGrade grade_ceiling = ConfidenceGrade::E_TransportOnly;
  CompletionAuthority authority = CompletionAuthority::TransportOnly;
  std::string method = "none";
  Provenance completion_provenance = Provenance::Unverified;

  // The drawer facet (docs/cash-drawer.md), classified rather than fired.
  bool drawer_present = false;
  bool drawer_kickable = false;
  DrawerPortStandard drawer_standard = DrawerPortStandard::Unknown;
  uint16_t drawer_voltage = 0;
  Provenance drawer_electrical_provenance = Provenance::Unverified;
  Provenance drawer_commands_provenance = Provenance::Unverified;

  // Declared degradations, in the words they are printed in — "BARCODE not supported on
  // this path" and its relatives. Produced by the DSL render report for the self-test
  // and by the classification itself for autoDetect. Empty means nothing was dropped.
  std::vector<std::string> degradations;

  // One line: "GS(H) fn48 probed - profile probed - identity untrusted (35%)".
  std::string provenanceSummary() const;
};

// docs/api.md §15. Everything is optional because the useful call is `selfTest()`.
struct SelfTestOptions {
  // Empty → "selftest-<unix ms>". A real idempotency key on a real job: running the
  // same key twice does not print twice, which is the same rule every other job obeys.
  std::string key;

  // Interrogate the device now instead of using what is already known. Runs the same
  // capability probe addPrinter schedules — same code, same worker thread, behind
  // whatever is queued — so a self-test can be the first thing that ever asks.
  bool refresh_identity = false;
  // The probe's own test lines print. False keeps the refresh printless, at the cost of
  // asking the ordered fences out of an empty buffer.
  bool probe_prints_test_lines = true;

  // The Code 128 sample. Omitted with a declared degradation on a path whose renderer
  // has no barcode command, never half-drawn.
  bool barcode = true;
  std::string barcode_data = "PD-SELFTEST";

  // The trailer QR and the `V:` line (docs/api.md §14). On, because the QR carrying the
  // job's own verification token is what makes this ticket evidence rather than a
  // printout.
  bool print_verification_id = true;

  // PC852 is the only code page with a full mapping table in this build, and every
  // letter of the Czech/Hungarian/Polish sample is in it. Overridable for a unit whose
  // firmware does not implement ESC t 18.
  escpos::CodePage code_page = escpos::CodePage::PC852;

  // The cut is deliberately not settable: the ticket ends in whatever this printer's
  // cutter natively does, because that is one of the things being tested.

  uint32_t timeout_ms = 0;  // 0 → the profile's completion budget
};

struct SelfTestResult {
  // The ordinary tri-state outcome of the ordinary engine. This is the proof.
  JobResult result;
  DetectionSummary detection;

  std::string key;
  // The ticket's own `V:` token, i.e. the four GS ( H characters printed on the paper
  // and inside the QR. Empty on a profile with no wire token to promote.
  std::string print_token;
  // The ticket as characters, exactly as it was laid out — the same layout that
  // produced the bytes, never a second one. For `pdctl self-test`, previews, and the
  // agent's response body.
  std::vector<std::string> ticket_lines;
  // The job handle, so a caller can subscribe, journal-query or resolve by token.
  std::shared_ptr<PrintJob> job;

  std::string ticketText() const;
};

// docs/api.md §15. NOTHING HERE PRINTS AND NOTHING FIRES.
struct AutoDetectOptions {
  // Empty → the local /24 (discovery.hpp localSubnet()). Ignored when `endpoints` is
  // non-empty.
  std::string subnet_cidr;
  // An explicit candidate list, "host" or "host:port". When non-empty the sweep is
  // skipped entirely and exactly these addresses are examined — the path a caller with
  // a known inventory takes, and the one that lets a test point at loopback ports.
  std::vector<std::string> endpoints;

  uint16_t port = 9100;
  uint32_t concurrency = 16;
  uint32_t connect_timeout_ms = 300;
  uint32_t response_timeout_ms = 400;

  // False leaves a device nobody has interrogated alone: cached findings are still
  // applied, and anything untouched comes back DetectionStatus::Unverified rather than
  // being asked. True runs the PRINTLESS probe subset described below.
  bool probe_unknown = true;

  // Per-phase probe budgets, forwarded to ProbeOptions.
  uint32_t status_timeout_ms = 700;
  uint32_t identity_timeout_ms = 700;
  uint32_t completion_timeout_ms = 1200;

  // -- Why the completion finding here is weaker than `pdctl probe`'s ----------------
  //
  // The full capability probe prints two short test lines, because an ordered fence
  // only means anything when there is print data ahead of it for the device to finish
  // first (docs/techspec.md §3.2). autoDetect may not print, so it runs the probe with
  // print_test_lines = false: the GS ( H marker and the GS r 1 query go out behind an
  // EMPTY buffer. A device that echoes them has proved that it *implements* the
  // command; it has not proved that the echo waits for paper to move. That is a real
  // finding and a weaker one, and it is reported as such — completion_provenance stays
  // Unverified on a printless answer unless cached findings from a real probe say
  // otherwise. FULL PROMOTION STILL NEEDS THE PRINTING PROBE (`pdctl probe`) OR A REAL
  // JOB. This field exists only so the restriction can be lifted deliberately.
  bool allow_printing_probe = false;
};

// One candidate, classified. `summary` carries the detail; the fields promoted onto this
// struct are the columns `pdctl autodetect` tabulates.
struct DetectedPrinter {
  std::string endpoint;  // "192.168.1.101:9100"
  std::string host;
  uint16_t port = 9100;

  DetectionStatus status = DetectionStatus::Unreachable;
  bool port_open = false;
  // Whatever came back from DLE EOT 1 during the sweep, verbatim and unclassified.
  std::vector<uint8_t> dle_eot_response;
  // True when the classification came from the findings store rather than from bytes
  // exchanged in this call.
  bool from_cache = false;

  DetectionSummary summary;

  const DetectedIdentity& identity() const noexcept { return summary.identity; }
  const std::string& profileId() const noexcept { return summary.profile_id; }
  CompletionMechanism completion() const noexcept { return summary.completion; }
  ConfidenceGrade gradeCeiling() const noexcept { return summary.grade_ceiling; }
};

// Called as each candidate is finished, from a worker thread. It must not block and must
// not call back into the driver.
using AutoDetectProgressCallback =
    std::function<void(const DetectedPrinter&, uint64_t completed, uint64_t total)>;

namespace detail {

// The one place a CapabilityProfile plus an IdentityAssessment becomes a report.
// Shared, so `pdctl autodetect`, `pdctl self-test`, the agent and the paper itself can
// never disagree about what the same device is. Not a public API: it is here because
// selfTest is compiled in the DSL library and needs it.
DetectionSummary summarize(const std::string& endpoint, const CapabilityProfile& profile,
                           const IdentityAssessment& identity);

// Which provenance column governs the mechanism actually in force. Reading the wrong
// column is how "GS ( H is documented for Epson" becomes a claim about a clone.
Provenance completionProvenanceOf(const CapabilityProfile& profile) noexcept;

ProfileSelection selectionFor(const CapabilityProfile& profile) noexcept;

}  // namespace detail

}  // namespace pd
