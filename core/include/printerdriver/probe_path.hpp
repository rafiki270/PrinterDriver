#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "printerdriver/transport.hpp"
#include "printerdriver/types.hpp"

// Print-server path probe (docs/compatibility-brief.md §19-§23).
//
// A USB/Ethernet print server box — or any other 9100 forwarder — moves the evidence
// boundary. The host stops talking to a printer and starts talking to something that
// talks to a printer, and §21 is explicit about what that costs: "Dumb USB/Ethernet
// servers: never equate TCP completion with printer completion. Probe whether DLE EOT's
// return byte traverses printer -> USB -> server -> TCP -> host, then a correlated fence;
// if responses don't survive: completionAuthority = PRINT_SERVER_ONLY."
//
// This file is that probe and nothing else. It writes two questions through the path
// under test and classifies what comes back:
//
//   DLE EOT 1     real-time, answered by the printer the moment it is scanned
//                 (docs/techspec.md §3.3). Its return byte is the cheapest possible test
//                 of whether the path carries printer -> host bytes at all. Useless as a
//                 completion fence, which is exactly why it is safe to ask here.
//   GS ( H fn 48  ONE marker carrying a token this probe chose. Its echo is the only
//                 answer in ESC/POS that is *correlated*: it names the receipt it belongs
//                 to (docs/techspec.md §3.1). A path that carries the real-time byte but
//                 loses the correlated echo cannot support a per-receipt completion
//                 claim, and that is a different finding from a path that carries
//                 nothing at all.
//
// Deliberately absent: any judgement about the printer's capabilities. A missing fn 48
// echo may mean the device has no fn 48, or that the box in the middle ate it, and no
// probe standing at one end of the path can separate those. So both readings are
// reported rather than one being picked — docs/compatibility-brief.md §28 keeps what a
// probe established and what it did not in two columns, never collapsed into one.
//
// Nothing printable is written unless the caller asks for it (PathProbeOptions::
// print_test_line): a port-9100 device prints what it receives, and a diagnostic that
// costs a venue paper every time it runs is a diagnostic nobody runs.

namespace pd {

// Four printable bytes for the correlated marker, chosen to sit outside both the range
// the engine's MarkerAllocator hands out and the capability probe's own "PRB0", so an
// echo arriving here can never be mistaken for a job's completion or for a capability
// probe running against the same device.
inline constexpr char kPathProbeToken[] = "PTH0";

struct PathProbeOptions {
  std::string host;
  uint16_t port = 9100;

  // Per phase, because the two questions have nothing in common. A real-time answer is
  // produced the instant the printer scans the query, so a budget measured in hundreds of
  // milliseconds is generous; the fn 48 echo is produced only once the device has
  // finished the data ahead of it, so it gets the print-job-sized budget.
  uint32_t connect_timeout_ms = 3000;
  uint32_t status_timeout_ms = 1500;
  uint32_t fence_timeout_ms = 8000;

  // Four printable-ASCII bytes (0x20..0x7E). Anything else is ignored in favour of
  // kPathProbeToken rather than throwing: the token that was actually issued is reported
  // back in PathProbeFindings::token, so a substitution is visible and not silent.
  std::string token;

  // Two short lines ahead of the marker. Off by default — see the header note about
  // paper. Turning it on makes the fence a stronger question (the device has something to
  // finish before it may answer) at the cost of a strip of till roll.
  bool print_test_line = false;

  // Overrides Transport::describe() in the report, for callers that know the path by a
  // better name than its socket.
  std::string endpoint;
};

// Everything the path probe observed first-hand, kept separate from the verdict so the
// verdict can be recomputed — and unit-tested — from the observation alone.
struct PathProbeFindings {
  std::string endpoint;
  std::string token;  // the marker token actually issued

  bool connected = false;
  std::string connect_error;
  // Whether each write was accepted by the path. A path that stops accepting bytes
  // half-way through has not answered the question this probe asks, and saying so is not
  // the same as saying the far side is silent.
  bool wrote_status_query = false;
  bool wrote_fence = false;
  std::string write_error;
  // The far side closed the link before the probe was done asking. Worth its own field
  // rather than being folded into write_error: a forwarder that accepts a job and then
  // hangs up is the textbook shape of the §21 box, and an operator reading the report
  // should see it named.
  bool link_dropped = false;
  std::string drop_message;

  // A real-time status answer was parsed out of the return stream. Kept alongside the raw
  // bytes because on an unfamiliar path the bytes nobody could classify are the
  // interesting part, and a verdict must never quietly discard them.
  bool dle_eot_answered = false;
  std::vector<uint8_t> dle_eot_response;

  // The GS ( H echo came back carrying the token this probe issued. Nothing weaker counts:
  // an echo is evidence only because it is correlated (docs/techspec.md §3.1).
  bool fence_echoed = false;
  // A structurally valid GS ( H echo carrying a token this probe never issued — another
  // writer on the same printer (docs/sdk-spec.md §14). Reported, never counted as an
  // answer to this probe's question.
  bool foreign_token_echoed = false;
  std::vector<std::string> foreign_tokens;

  // Everything the far side sent, in order, across both phases.
  std::vector<uint8_t> raw;
  // Bytes the response parser could not classify as any known answer.
  std::vector<uint8_t> unclassified;

  uint32_t connect_ms = 0;
  uint32_t status_ms = 0;
  uint32_t fence_ms = 0;

  // Filled by probePath() from classifyPath()/explainPath(). Carried on the findings so a
  // report, a log line and an exit status all quote the same verdict.
  CompletionAuthority authority = CompletionAuthority::TransportOnly;
  std::string rationale;

  // The path carried at least one byte back from the far side.
  bool pathCarriesResponses() const noexcept { return !raw.empty(); }
  // The §21 case, in one predicate: bytes went out, nothing came back.
  bool responsesSwallowed() const noexcept {
    return connected && wrote_status_query && wrote_fence && raw.empty();
  }
  // DLE EOT answered but the correlated echo never arrived. A genuinely different finding
  // from responsesSwallowed(): here the backchannel demonstrably works.
  bool correlationLostOnly() const noexcept {
    return connected && wrote_status_query && wrote_fence && !fence_echoed &&
           dle_eot_answered;
  }
};

// docs/compatibility-brief.md §21 names the print-server verdict in these exact words:
// `completionAuthority = PRINT_SERVER_ONLY`. The other members are spelled the same way,
// because a report that rendered only one of them in the brief's own vocabulary would
// make that one look like a different kind of value than the alternatives it is being
// compared against.
const char* pathAuthorityLabel(CompletionAuthority) noexcept;

// The whole rule table, as a pure function of the observation:
//
//   path never opened, or stopped accepting bytes    -> TransportOnly
//   correlated fn 48 echo came back with our token   -> PhysicalPrinter
//   every byte accepted and nothing came back at all -> PrintServer  (§21)
//   anything came back but not our correlated echo   -> TransportOnly
//
// The last two lines are the distinction this command exists to make. "Silent" and
// "answered, but not about my receipt" are different facts about a path, and only the
// first of them means the printer's answers never make it back to the host.
CompletionAuthority classifyPath(const PathProbeFindings& observation) noexcept;

// One sentence a support engineer can act on six months later, always ending in
// "completionAuthority = <label>" so the verdict survives being quoted out of context.
std::string explainPath(const PathProbeFindings& observation);

// Drives the probe over an already-constructed transport: connects it, asks both
// questions, closes it, and returns the classified findings. Taking a Transport& rather
// than a socket is what lets the classification be tested against a scripted device
// instead of a print server nobody owns.
PathProbeFindings probePath(Transport& transport, const PathProbeOptions& options = {});

// Convenience overload for callers holding an address rather than a transport.
PathProbeFindings probePath(const PathProbeOptions& options);

}  // namespace pd
