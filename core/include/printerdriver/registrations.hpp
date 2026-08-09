#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/types.hpp"

// M16 — custom method registration (docs/api.md §16).
//
// Five runtime extension points, all per-driver-instance, all data-plus-callbacks (no
// subclassing across the ABI), all keyed by namespaced string ids, all process-local
// (never persisted into a shared journal beyond their ids), and all id-attributed in the
// results they produce so a custom method's claims are auditable exactly like a built-in.
//
// The callbacks below are std::function wrappers. The C ABI (pd.h) binds each to a plain
// C function pointer plus a void* context, exactly as pd_add_printer_custom binds the
// transport vtable; every wrapper marshals its own closures/delegates into those pointers.
//
// -- Callback thread contract ---------------------------------------------------------
//
// Every callback here is invoked on a CORE thread, never the caller's — the same
// contract the custom-transport vtable documents (transport.hpp, pd.h "Callback
// threads"):
//   * a completion method's fence_bytes / matcher run on the owning printer's worker
//     thread and its transport reader path, one flow at a time;
//   * a probe step's classify runs on the worker thread that drives the probe;
//   * a drawer kick's kick_bytes / status_parse run on the worker thread;
//   * a formatter / block handler runs on whatever thread renders the document.
// A callback must not block and must not re-enter any pd_* function on the same driver.

namespace pd {

// --- (1) Custom completion mechanism -------------------------------------------------
//
// The engine, when a printer's profile completion is CompletionMechanism::VendorIdle
// bound to a registered id, sends fence_bytes(job_token) behind the payload and routes
// the continuous printer->host stream through matcher. A Matched(token) confirms exactly
// like GS ( H: the token is looked up in the very same per-job marker map, so the same
// attribution, the same journalled RVI and the same jobByToken/pdctl verify resolution
// all apply. The registered grade/authority/method_name ride on the JobResult.

enum class CustomMatchKind {
  // The stream carried a fence answer whose correlation token is `token`. The engine
  // confirms the job whose outstanding fence token matches it, exactly like a GS ( H echo.
  Matched,
  // These bytes are not this mechanism's — the engine keeps them for the built-in status
  // parser and clears the matcher's own buffer.
  NotMine,
  // A fence answer may be forming but is incomplete; the engine keeps buffering and
  // re-invokes the matcher when more bytes arrive.
  NeedMore,
};

struct CustomMatch {
  CustomMatchKind kind = CustomMatchKind::NeedMore;
  std::string token;  // 4-char correlation token, only meaningful for Matched

  static CustomMatch matched(std::string token) {
    return CustomMatch{CustomMatchKind::Matched, std::move(token)};
  }
  static CustomMatch notMine() { return CustomMatch{CustomMatchKind::NotMine, {}}; }
  static CustomMatch needMore() { return CustomMatch{CustomMatchKind::NeedMore, {}}; }
};

struct CompletionMethod {
  std::string id;  // namespaced, e.g. "acme.x-idle"
  // Bytes to send behind the payload for this job's four-character verification token.
  std::function<escpos::Bytes(const std::string& job_token)> fence_bytes;
  // Classify the accumulated printer->host bytes since the last Matched/NotMine.
  std::function<CustomMatch(const uint8_t* data, size_t size)> matcher;
  // What a confirmed completion on this method claims (attributed by id in results).
  ConfidenceGrade grade = ConfidenceGrade::A_JobLevelConfirmation;
  CompletionAuthority authority = CompletionAuthority::PhysicalPrinter;
  std::string method_name;  // the string a support engineer reads, e.g. "acme.x-idle"

  bool valid() const noexcept {
    return !id.empty() && fence_bytes && matcher && !method_name.empty();
  }
};

// --- (2) Custom probe step -----------------------------------------------------------
//
// Extends probe/autoDetect fingerprinting. request_bytes MUST be non-printing — a step
// that put printable bytes on the wire would cost a venue a roll of paper on every
// autoDetect sweep — so isNonPrintingRequest() gates registration.

struct ProbeFinding {
  bool answered = false;  // the device replied to this step at all
  std::string label;      // short classification, surfaced in the findings summary
};

struct ProbeStep {
  std::string id;  // namespaced
  std::vector<uint8_t> request_bytes;
  std::function<ProbeFinding(const std::vector<uint8_t>& response)> classify;

  bool valid() const noexcept { return !id.empty() && classify; }
};

// True when NONE of `bytes` can print on any ESC/POS device: every byte is below 0x20 or
// above 0x7E, and none is a line feed. The exact inverse of a printable run — the same
// rule pd_discover's DLE EOT sweep relies on to never cost paper.
bool isNonPrintingRequest(const std::vector<uint8_t>& bytes) noexcept;

// --- (5) Custom drawer kick method ---------------------------------------------------
//
// Fills DrawerKickMethod::Vendor for a profile. kick_bytes produces the pulse; the
// optional status pair reads the switch back where the profile documents one.

struct DrawerKickReg {
  std::string id;  // namespaced
  std::function<escpos::Bytes(uint8_t channel, uint16_t pulse_ms)> kick_bytes;
  // Optional. status_request returns the bytes that ask for the switch state; status_parse
  // turns the reply into pin-high / pin-low, or nullopt when it could not be read. Both
  // empty means "no readable switch on this vendor method".
  std::function<escpos::Bytes()> status_request;
  std::function<std::optional<bool>(const std::vector<uint8_t>& response)> status_parse;

  bool valid() const noexcept { return !id.empty() && kick_bytes; }
  bool readable() const noexcept {
    return static_cast<bool>(status_request) && static_cast<bool>(status_parse);
  }
};

// --- (4) Custom formatter ------------------------------------------------------------
//
// Backs {{ v | name:args }} in the DSL template layer, checked before the built-in
// formatter table. nullopt means "not mine" — fall through to the built-ins.

struct FormatterReg {
  std::string name;
  std::function<std::optional<std::string>(const std::string& value,
                                           const std::string& args,
                                           const std::string& locale)>
      fn;

  bool valid() const noexcept { return !name.empty() && fn; }
};

// --- (3) Custom document block handler -----------------------------------------------
//
// A new DSL block kind renders through the ordinary pd::dsl pipeline. Unknown block kinds
// otherwise fail the parse; a registered handler intercepts the kind first and either
// emits ESC/POS ops or declares a degradation the same way a built-in block does.

struct BlockRenderResult {
  bool ok = false;   // true -> splice `bytes`; false -> a degradation entry
  escpos::Bytes bytes;
  // Degradation text (used when !ok), the words it is printed/reported in.
  std::string requested;
  std::string delivered = "omitted";
  std::string detail;
};

struct BlockHandlerReg {
  std::string kind;  // the block object key that selects this handler, e.g. "loyaltyStamp"
  // block_json is the block object; profile_json is a small JSON view of the render
  // profile facts (width_dots, barcode/qr/raster support).
  std::function<BlockRenderResult(const std::string& block_json,
                                  const std::string& profile_json)>
      fn;

  bool valid() const noexcept { return !kind.empty() && fn; }
};

// Non-empty, no whitespace or control characters. The spec's ids are namespaced strings
// like "acme.x-idle"; this is the floor every registration point shares.
bool isValidRegistrationId(const std::string& id) noexcept;

// Per-driver-instance registries (docs/api.md §16). Thread-safe; registrations are
// add-only for the life of the driver, which is what lets a matcher/fence pointer handed
// to a worker thread stay valid without a lock on the hot path.
class Registrations {
 public:
  // Each add* returns false and fills `error` on a bad id, a duplicate, or an invalid
  // record (a null required callback, a printing probe request). *error may be null.

  bool addCompletionMethod(CompletionMethod method, std::string* error);
  std::optional<CompletionMethod> completionMethod(const std::string& id) const;

  bool addProbeStep(ProbeStep step, std::string* error);
  std::vector<ProbeStep> probeSteps() const;

  bool addDrawerKick(DrawerKickReg kick, std::string* error);
  std::optional<DrawerKickReg> drawerKick(const std::string& id) const;

  bool addFormatter(FormatterReg formatter, std::string* error);
  bool hasFormatter(const std::string& name) const;
  // nullopt when no formatter of that name is registered, or the registered one declined.
  std::optional<std::string> format(const std::string& name, const std::string& value,
                                    const std::string& args,
                                    const std::string& locale) const;

  bool addBlockHandler(BlockHandlerReg handler, std::string* error);
  bool hasBlockHandler(const std::string& kind) const;
  std::optional<BlockRenderResult> renderBlock(const std::string& kind,
                                               const std::string& block_json,
                                               const std::string& profile_json) const;

 private:
  mutable std::mutex mutex_;
  std::vector<CompletionMethod> completions_;
  std::vector<ProbeStep> probe_steps_;
  std::vector<DrawerKickReg> drawer_kicks_;
  std::vector<FormatterReg> formatters_;
  std::vector<BlockHandlerReg> block_handlers_;
};

}  // namespace pd
