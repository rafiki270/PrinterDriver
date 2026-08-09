#pragma once

#include <string>
#include <string_view>

#include "printerdriver/driver.hpp"
#include "printerdriver/dsl/bind.hpp"
#include "printerdriver/dsl/document.hpp"
#include "printerdriver/dsl/render.hpp"
#include "printerdriver/dsl/report.hpp"
#include "printerdriver/registrations.hpp"

// M19 — the one path from "a JSON receipt document" to "ESC/POS bytes plus a render
// report" (docs/api.md §3, docs/receipt-dsl.md).
//
// parseDocument → bind → render has always been three calls in a fixed order with a
// fixed set of ways to go wrong, and every caller that walked it by hand had to get the
// same five things right:
//
//   1. fold the parser's warnings into the report rather than dropping them;
//   2. refuse a template with no model instead of printing its placeholders;
//   3. wire the driver's registrations (docs/api.md §16) into ALL THREE stages — the
//      parser needs them to accept a registered block kind, the binder to resolve
//      {{ v | name }}, the renderer to draw the block — because a registration wired
//      into two of three is a registration that silently does not fire;
//   4. merge three reports in the order the problems actually happened;
//   5. apply the document's meta with the caller's own JobOptions winning over it.
//
// Written once here so `pdctl print --template`, `pd_render_document` and
// `pd_print_document_json` cannot disagree about what a document means. This layer owns
// no socket and no job: it is a pure function of (document JSON, model JSON, profile),
// exactly like the renderer beneath it.

namespace pd::dsl {

struct DocumentPipelineOptions {
  RenderOptions render;
  BindOptions bind;
  // Set on all three stages at once, overwriting RenderOptions::registrations and
  // BindOptions::registrations. nullptr leaves both fields as the caller set them.
  const ::pd::Registrations* registrations = nullptr;
};

struct DocumentPipelineOutcome {
  // False only for the failures that have no receipt on the other side: JSON that is not
  // JSON, a structure that is not a document, a template with no model. Every softer
  // problem — a missing model path, an unknown formatter, a barcode the profile cannot
  // draw — is an entry in `report`, and the receipt still prints. That is the
  // degradation contract of docs/receipt-dsl.md, not a convenience.
  bool ok = false;
  // Why there are no bytes. Empty when `ok`.
  std::string error;
  Document document;  // bound; the template flag is cleared
  RenderOutput output;
  // Parse warnings, then the binder's report, then the renderer's — the order they
  // happened in. NEVER EMPTY WHEN `ok` IS FALSE: a caller that cannot print is told what
  // went wrong through the same channel it reads degradations from, so an error and a
  // degradation do not need two different displays.
  RenderReport report;
};

// Parses, binds when `model_json` is non-null, and renders. Never throws: a document
// that cannot be parsed comes back with ok == false and a MalformedTemplate entry.
DocumentPipelineOutcome renderDocumentJson(std::string_view document_json,
                                           const std::string* model_json,
                                           const DocumentPipelineOptions& options = {});

// docs/receipt-dsl.md "Cut control" and "Margins": caller > document meta > profile.
//
// A `cut` still at CutSetting::Profile and a feed still at 0 are what "the caller said
// nothing" looks like in JobOptions, so the document's own meta fills them in; anything
// the caller set explicitly is left alone. The engine applies its blade-clearance floor
// on top of whatever comes out of here — it feeds max(profile floor, bottom_feed_dots) —
// so no document can ask for less clearance than the hardware needs and reintroduce a
// clipped trailing QR.
void applyDocumentMeta(const RenderOutput& output, JobOptions* options);

}  // namespace pd::dsl
