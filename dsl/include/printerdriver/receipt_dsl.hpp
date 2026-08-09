#pragma once

// The receipt DSL (docs/receipt-dsl.md), assembled: document model, strict JSON,
// template binding, value formatters and the hardware-path renderer.
//
//   pd::dsl::parseDocument(json)          → Document          (document.hpp)
//   pd::dsl::bind(document, model)        → BindOutcome       (bind.hpp)
//   pd::dsl::render(document, options)    → RenderOutput      (render.hpp)
//   pd::dsl::renderText(document, opts)   → TextPreview       (render.hpp)
//   pd::dsl::renderDocumentJson(...)      → DocumentPipelineOutcome  (pipeline.hpp)
//
// The last of those is the three above in their fixed order with their fixed failure
// modes handled once (M19); it is what `pdctl --template` and the C ABI's
// pd_render_document / pd_print_document_json all call, and what a new caller should
// reach for before assembling the steps by hand.
//
// One include for callers that want all of it; the individual headers stay usable on
// their own, because a wrapper that only serializes documents should not have to
// compile the renderer.

#include "printerdriver/dsl/barcode.hpp"
#include "printerdriver/dsl/bind.hpp"
#include "printerdriver/dsl/document.hpp"
#include "printerdriver/dsl/format.hpp"
#include "printerdriver/dsl/json.hpp"
#include "printerdriver/dsl/pipeline.hpp"
#include "printerdriver/dsl/render.hpp"
#include "printerdriver/dsl/report.hpp"
#include "printerdriver/dsl/text.hpp"
