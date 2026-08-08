#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "printerdriver/escpos_encoder.hpp"
#include "printerdriver/types.hpp"

// Per model/firmware capability data (docs/sdk-spec.md §8). A profile is plain data,
// determined by the probe in docs/testing-plan.md and shipped with the SDK; the engine
// reads it and never guesses. Its most important job is deciding which ConfidenceLevel
// a job on that printer can ever reach, so the SDK reports what the hardware can prove
// instead of what the caller would like to hear.

namespace pd {

// Which ordered completion fence this printer answers (docs/techspec.md §3).
enum class CompletionMechanism {
  GsParenH,  // GS ( H fn 48 process-ID echo — per-receipt correlation token
  GsR1,      // GS r 1 queued paper status — completion, but anonymous
  None,      // write-only device: no backchannel, no ordered fence
};

// The cut this printer's mechanism actually performs.
enum class CutVariant { Partial, Full, None };

struct CapabilityProfile {
  std::string name = "generic-escpos";

  CompletionMechanism completion = CompletionMechanism::GsR1;

  // Native cut for this mechanism; used when JobOptions asks for CutSetting::Profile.
  CutVariant cut = CutVariant::Partial;
  // GS V 65/66 n instead of GS V m: feeds to the cut position first. The workaround
  // for clones that cut into the last printed line (docs/sdk-spec.md §9, Rongta).
  bool cut_with_feed = false;
  uint8_t cut_feed_units = 0;

  bool supports_asb = true;
  // Whether DLE EOT answers come back at all. False disables preflight, because a
  // preflight that can never be answered is a guaranteed timeout, not a check.
  bool supports_realtime_status = true;

  // Flow control. chunk_bytes == 0 disables chunking; inter_chunk_delay_ms == 0
  // disables pacing. Both off is the correct setting for a printer with real flow
  // control — the 9-21 ms delays in the legacy stack were empirical, not universal.
  size_t chunk_bytes = 0;
  uint32_t inter_chunk_delay_ms = 0;

  // Budget for one completion wait (marker echo or queued status), not for the job.
  uint32_t completion_timeout_ms = 15000;
  uint32_t preflight_timeout_ms = 2000;

  // Lines fed after the payload so the job ends in a genuine print-and-feed
  // operation, which is what the ordered fence attaches to (docs/techspec.md §5.2).
  uint8_t final_feed_lines = 4;

  escpos::CodePage code_page = escpos::CodePage::PC437;

  // docs/sdk-spec.md §8: the ceiling this hardware allows. The engine clamps every
  // reported confidence to it.
  ConfidenceLevel maxConfidence() const noexcept;
};

// Xprinter XP-S260M as probed on 2026-08-08 over LAN
// (docs/testing-plan.md — GS ( H confirmed, DLE EOT and GS r 1 also answer).
CapabilityProfile xp_s260m();

// The conservative default for anything not yet probed: queued GS r 1 fence, pacing
// on, so a cheap clone with no flow control still receives a whole receipt.
CapabilityProfile generic_escpos();

const char* to_string(CompletionMechanism) noexcept;
const char* to_string(CutVariant) noexcept;

}  // namespace pd
