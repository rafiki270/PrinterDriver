#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "printerdriver/dsl/json.hpp"

// The render report (docs/receipt-dsl.md "Degradation rules (closed, declared)"):
//
//   renderReport: [{block, requested, delivered, path: hardware|raster}]
//
// Every departure from what the document asked for lands here. Nothing in this library
// throws once a document has been parsed — a missing model path, an unknown formatter,
// an unrenderable barcode and a style the hardware cannot produce all become entries and
// the receipt still prints. That is the same honesty contract as the job outcome: a
// declared degradation is not a failure, but it is never silent either.

namespace pd::dsl {

enum class RenderPath {
  Hardware,     // produced by ESC/POS commands
  Raster,       // produced as an image (not available in this milestone)
  NotRendered,  // requested, deliberately absent from the output
};

enum class ReportKind {
  MissingPath,          // {{order.note}} with no such path in the model
  UnknownFormatter,     // {{v|frobnicate}}
  UnformattableValue,   // {{v|number:2}} where v is an object
  MalformedTemplate,    // unterminated {{ or an empty path
  UnknownStyle,         // style: "h1" with no such named style
  StyleCycle,           // extends chain that loops
  UnsupportedStyle,     // italic, rotate90, a raster font on the hardware path
  UnsupportedBlock,     // barcode in this milestone, or an unbound control block
  MissingImage,         // image ref with no asset supplied
  Truncated,            // content clipped to fit the media width
  EmptyIteration,       // each over a missing or non-array path
  UnsupportedTimezone,  // an IANA zone name: this library has no tz database
  RawFramingRisk,       // a raw block carrying a cut or a status command
  Note,                 // everything else worth telling the operator
};

struct ReportEntry {
  ReportKind kind = ReportKind::Note;
  // Where it happened, as a path into the document: "blocks[3].cells[1]", "meta.tz".
  std::string block;
  std::string requested;
  std::string delivered;
  RenderPath path = RenderPath::Hardware;
  std::string detail;
};

struct RenderReport {
  std::vector<ReportEntry> entries;

  void add(ReportKind kind, std::string block, std::string requested,
           std::string delivered, RenderPath path = RenderPath::Hardware,
           std::string detail = {});
  void append(const RenderReport& other);

  bool empty() const noexcept { return entries.empty(); }
  size_t size() const noexcept { return entries.size(); }
  size_t count(ReportKind kind) const noexcept;
  bool has(ReportKind kind) const noexcept { return count(kind) != 0; }

  // One line per entry, for pdctl and for test failure messages.
  std::string toText() const;
  // The wire form: an array of {block, requested, delivered, path, kind, detail}.
  Json toJson() const;
};

const char* to_string(ReportKind kind) noexcept;
const char* to_string(RenderPath path) noexcept;

}  // namespace pd::dsl
