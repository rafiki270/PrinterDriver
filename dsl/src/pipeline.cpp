#include "printerdriver/dsl/pipeline.hpp"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include "printerdriver/dsl/json.hpp"

namespace pd::dsl {
namespace {

// JobOptions carries its feeds as uint16_t, and the encoder's ESC J chunking cannot
// express more. Clamped rather than refused, for the same reason toOptions() in the C
// ABI clamps: a margin wider than a roll is a caller asking for too much whitespace, and
// losing a ticket over presentation is the wrong trade.
uint16_t clampFeed(uint32_t dots) noexcept {
  return static_cast<uint16_t>(std::min<uint32_t>(dots, 0xFFFFu));
}

}  // namespace

DocumentPipelineOutcome renderDocumentJson(std::string_view document_json,
                                           const std::string* model_json,
                                           const DocumentPipelineOptions& options) {
  DocumentPipelineOutcome out;

  DocumentPipelineOptions effective = options;
  if (effective.registrations != nullptr) {
    effective.render.registrations = effective.registrations;
    effective.bind.registrations = effective.registrations;
  }

  // 1. Parse. Structural failures throw; unknown vocabulary does not, so a template
  //    written against a later revision of the spec still prints what this build
  //    understands and says which keys it ignored.
  std::vector<std::string> warnings;
  Document document;
  try {
    document = parseDocument(document_json, &warnings, effective.render.registrations);
  } catch (const std::exception& error) {
    out.error = std::string("document rejected: ") + error.what();
    out.report.add(ReportKind::MalformedTemplate, "document", "a receipt document",
                   "nothing", RenderPath::NotRendered, error.what());
    return out;
  }
  for (const std::string& warning : warnings) {
    out.report.add(ReportKind::Note, "document", warning, "ignored", RenderPath::Hardware);
  }

  // 2. Bind, when there is a model. A template with none is refused rather than printed:
  //    a receipt reading "{{order.total}}" is worse than no receipt, because it looks
  //    like one.
  if (model_json == nullptr) {
    if (document.is_template) {
      out.error = "this document is a template: a parameter model is required";
      out.report.add(ReportKind::MalformedTemplate, "document", "a bound template",
                     "nothing", RenderPath::NotRendered,
                     "no parameter model was supplied");
      return out;
    }
  } else {
    Json model;
    std::string error;
    if (!tryParseJson(*model_json, &model, &error)) {
      out.error = "model rejected: " + error;
      out.report.add(ReportKind::MalformedTemplate, "model", "a parameter model",
                     "nothing", RenderPath::NotRendered, error);
      return out;
    }
    BindOutcome bound = bind(document, model, effective.bind);
    out.report.append(bound.report);
    document = std::move(bound.document);
  }

  // 3. Render against the media. Everything from here on is a declared degradation, not
  //    a failure.
  out.output = render(document, effective.render);
  out.report.append(out.output.report);
  out.document = std::move(document);
  out.ok = true;
  return out;
}

void applyDocumentMeta(const RenderOutput& output, JobOptions* options) {
  if (options == nullptr) {
    return;
  }

  if (options->cut == CutSetting::Profile && output.requested_cut.has_value()) {
    switch (*output.requested_cut) {
      case CutRequest::Profile: options->cut = CutSetting::Profile; break;
      case CutRequest::Partial: options->cut = CutSetting::Partial; break;
      case CutRequest::Full: options->cut = CutSetting::Full; break;
      case CutRequest::None: options->cut = CutSetting::None; break;
    }
  }

  if (options->top_feed_dots == 0 && output.requested_margins.top_dots.has_value()) {
    options->top_feed_dots = clampFeed(*output.requested_margins.top_dots);
  }
  if (options->bottom_feed_dots == 0 && output.requested_margins.bottom_dots.has_value()) {
    options->bottom_feed_dots = clampFeed(*output.requested_margins.bottom_dots);
  }
}

}  // namespace pd::dsl
