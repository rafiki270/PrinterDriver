#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "printerdriver/dsl/document.hpp"
#include "printerdriver/dsl/format.hpp"
#include "printerdriver/dsl/json.hpp"
#include "printerdriver/dsl/report.hpp"

// Template binding (docs/receipt-dsl.md "Templates + parameter models"):
//
//   {{path.to.value}}  substitution with escaping
//   {"each": "order.items", "blocks": [...]}   repeats a block *list*, scope = element
//   {"if": "order.note", "block": {...}}       / {"unless": ...} on truthiness
//
// Deliberately logic-less: no expressions, no arithmetic, no conditionals beyond
// truthiness. The app prepares the model; the template only lays it out.
//
// Binding never throws. A missing path becomes an empty string plus a render-report
// warning, an unknown formatter leaves the raw value plus a warning, and a malformed
// `{{` is reported and left alone — because the alternative, half a receipt and an
// exception, is exactly the failure mode this SDK exists to eliminate.
//
// Three rules the spec leaves open, resolved here and testable:
//
// * **Scope walks outward.** A path is resolved against the innermost `each` element
//   first, then each enclosing scope, then the model root (Mustache semantics). Inside
//   an `each`, `{{name}}` is the element's name and `{{venue.name}}` still reaches the
//   root. `{{.}}` / `{{this}}` is the element itself; `{{@index}}` is its 0-based
//   position.
// * **`if`/`unless` do not warn on a missing path.** Optionality is the entire point of
//   a conditional; warning about it would bury the warnings that matter.
// * **Substituted values are sanitised.** C0 control bytes are stripped from every
//   bound value, so a model field can never inject ESC/POS commands into a receipt
//   built from a trusted layout. That is what "substitution with escaping" has to mean
//   on a device where the data channel is also the command channel.
// * **A path that lands on an array or an object still prints** — as compact JSON, with
//   an UnformattableValue entry. A line of text cannot be a structure, and printing
//   nothing would hide the mistake instead of showing it.

namespace pd::dsl {

struct BindOptions {
  FormatTables tables = FormatTables::builtin();
  // Non-empty values win over the document's own meta.
  std::string locale;
  std::string currency;
  std::string tz;
  // Guards against a runaway model: a 200 000-line receipt is a defect, not a receipt.
  size_t max_blocks = 20000;
  size_t max_depth = 32;
};

struct BindOutcome {
  Document document;    // template flag cleared: the result is a plain document
  RenderReport report;
};

BindOutcome bind(const Document& document, const Json& model,
                 const BindOptions& options = {});

// One string, bound against the model root. Exposed because it is the smallest unit
// worth testing and the one wrappers want for labels.
std::string bindString(std::string_view source, const Json& model,
                       const BindOptions& options = {}, RenderReport* report = nullptr,
                       const std::string& where = {});

// Resolves a dotted path against a model root; nullptr when absent.
const Json* lookupPath(const Json& model, std::string_view path);

}  // namespace pd::dsl
