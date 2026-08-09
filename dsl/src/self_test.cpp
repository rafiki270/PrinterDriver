#include "printerdriver/self_test.hpp"

#include <string>
#include <utility>
#include <vector>

#include "printerdriver/device_profiles.hpp"
#include "printerdriver/driver.hpp"
#include "printerdriver/dsl/document.hpp"
#include "printerdriver/dsl/render.hpp"
#include "printerdriver/job_store.hpp"

// M15 — the self-test ticket (docs/api.md §15).
//
// One diagnostic ticket, through the ordinary fenced engine, under an ordinary
// idempotency key. Nothing here talks to a socket, arms a fence or invents a byte: it
// gathers what the driver already knows, lays it out as a receipt-DSL document, and
// submits the result through Printer::print like any other job. The FENCE block on the
// paper names the mechanism this very ticket is being confirmed by, and the JobResult
// this function returns is the confirmation.
//
// -- Why this translation unit is in the DSL library ----------------------------------
//
// Because the ticket is a document, and the DSL depends on the core rather than the
// other way round. Two consequences are load-bearing rather than incidental:
//
//   * the charset line is laid out and transliterated by the same renderer every
//     receipt goes through, so a self-test that prints correctly is evidence about the
//     path real receipts take;
//   * the RenderReport's declared degradations are printed ON THE TICKET. A block the
//     profile cannot draw becomes a line an operator can read — "BARCODE not supported
//     on this path" — instead of a silence.
//
// See the note at the top of printerdriver/self_test.hpp for the linkage contract.

namespace pd {
namespace {

using dsl::Align;
using dsl::Block;
using dsl::Cell;
using dsl::CellWidth;
using dsl::Document;
using dsl::TextStyle;

// 11, not 10: the longest label is COMPLETION at ten characters, and a label column
// exactly as wide as its widest label prints `COMPLETIONGsParenH`.
constexpr int kLabelColumns = 11;

// Locale-free, tz-free, deterministic. The DSL refuses to call strftime or setlocale
// (dsl/include/printerdriver/dsl/format.hpp) and a diagnostic ticket is the last place
// to make an exception: the stamp is UTC, spelled out, and says so.
std::string utcStamp(uint64_t unix_ms) {
  const int64_t seconds = static_cast<int64_t>(unix_ms / 1000u);
  int64_t days = seconds / 86400;
  int64_t rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  // Howard Hinnant's civil_from_days, which is exact and has no table.
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = y + (m <= 2 ? 1 : 0);

  const auto pad2 = [](int64_t value) {
    std::string text = std::to_string(value);
    return text.size() < 2 ? "0" + text : text;
  };
  return std::to_string(year) + "-" + pad2(m) + "-" + pad2(d) + " " +
         pad2(rem / 3600) + ":" + pad2((rem / 60) % 60) + ":" + pad2(rem % 60) + "Z";
}

Block textBlock(std::string content, const std::string& style_name) {
  Block block;
  block.kind = Block::Kind::Text;
  block.content = std::move(content);
  if (!style_name.empty()) {
    block.style.name = style_name;
  }
  return block;
}

// LABEL + value, as a two-cell row. A columns row is what gives the continuation lines
// their hanging indent, which is why the FENCE paragraph in docs/api.md §15 lines up
// under its own text instead of under the label.
Block labelRow(const std::string& label, const std::string& value) {
  Block block;
  block.kind = Block::Kind::Columns;
  Cell left;
  left.content = label;
  left.width = CellWidth{CellWidth::Unit::Chars, kLabelColumns};
  Cell right;
  right.content = value;
  right.width = CellWidth{CellWidth::Unit::Flex, 1};
  block.cells.push_back(std::move(left));
  block.cells.push_back(std::move(right));
  return block;
}

Block divider() {
  Block block;
  block.kind = Block::Kind::Divider;
  block.divider = dsl::DividerKind::Solid;
  return block;
}

Block feed(int lines) {
  Block block;
  block.kind = Block::Kind::Feed;
  block.feed_lines = lines;
  return block;
}

std::string yesNo(bool value) { return value ? "YES" : "NO"; }

// One report entry as the single line it prints as. The barcode case is spelled out
// because it is the one docs/api.md §15 names, and because an operator reading it needs
// to know it is the path and not the data that refused.
std::string degradationLine(const dsl::ReportEntry& entry) {
  const bool barcode = entry.requested.rfind("barcode:", 0) == 0;
  const bool qr = entry.requested == "qr";
  std::string out;
  if (barcode) {
    out = "BARCODE not supported on this path";
  } else if (qr) {
    out = "QR not supported on this path";
  } else {
    out = std::string(dsl::to_string(entry.kind)) + " in " +
          (entry.block.empty() ? "document" : entry.block);
  }
  if (!entry.detail.empty()) {
    out += ": " + entry.detail;
  }
  return out;
}

// The whole ticket, as a document. Built twice: once to find out what the profile
// cannot draw, and once with those answers printed on it.
Document buildTicket(const DetectionSummary& summary, const SelfTestOptions& options,
                     const CapabilityProfile& profile, uint64_t stamp_ms,
                     const std::vector<std::string>& degradations) {
  Document doc;

  TextStyle title;
  title.align = Align::Center;
  title.bold = true;
  doc.setStyle("title", title);

  TextStyle subtitle;
  subtitle.align = Align::Center;
  doc.setStyle("subtitle", subtitle);

  TextStyle centered;
  centered.align = Align::Center;
  doc.setStyle("centered", centered);

  doc.blocks.push_back(textBlock("PRINTERDRIVER SELF-TEST", "title"));
  doc.blocks.push_back(
      textBlock(std::string("sdk ") + kSdkVersion + "  " + utcStamp(stamp_ms),
                "subtitle"));
  doc.blocks.push_back(divider());

  const DetectedIdentity& identity = summary.identity;
  std::string identity_text = identity.vendor;
  if (!identity.model.empty()) {
    identity_text += "/" + identity.model;
  }
  identity_text += " (GS I, trusted " + yesNo(identity.trusted) + ", " +
                   std::to_string(static_cast<int>(identity.confidence_percent)) + "%)";
  doc.blocks.push_back(labelRow("IDENTITY", identity_text));

  doc.blocks.push_back(labelRow(
      "PROFILE", summary.profile_id + " - selected by " + to_string(summary.selection)));

  doc.blocks.push_back(labelRow(
      "MEDIA", std::to_string(summary.printable_width_dots) + " dots  " +
                   std::to_string(summary.chars_per_line) + " cols  " +
                   std::to_string(summary.dpi) + " dpi  " +
                   std::to_string(summary.nominal_paper_mm) + " mm roll"));

  // The method is deliberately not repeated here: the FENCE row below names it, and
  // this row has to fit one line of 38 columns on 80 mm paper so an operator reads the
  // grade ceiling and its provenance side by side rather than across a wrap.
  doc.blocks.push_back(labelRow(
      "COMPLETION", std::string(to_string(summary.completion)) + " - grade ceiling " +
                        gradeLetter(summary.grade_ceiling) + ", " +
                        to_string(summary.completion_provenance)));

  // The point of the ticket. Every letter below is in PC852, which is the code page the
  // payload declares — a self-test printing question marks would be testing the wrong
  // thing (docs/api.md §15).
  doc.blocks.push_back(labelRow("CHARSET", kCharsetSample));

  if (options.barcode) {
    doc.blocks.push_back(labelRow("BARCODE", "Code 128 \"" + options.barcode_data + "\""));
    Block symbol;
    symbol.kind = Block::Kind::Barcode;
    symbol.symbology = dsl::Symbology::Code128;
    symbol.content = options.barcode_data;
    symbol.barcode_height_dots = 60;
    symbol.barcode_module_width = 2;
    symbol.hri = dsl::Hri::Below;
    symbol.align = Align::Center;
    doc.blocks.push_back(std::move(symbol));
  }
  if (options.print_verification_id) {
    // The trailer QR is the engine's, not this document's (docs/api.md §14): it carries
    // the very token this job's fence is echoing, which cannot be known until the job
    // reaches a worker. Naming it here is what tells an operator what the symbol at the
    // bottom of the paper is — and, on a printer with no wire token to promote, why
    // there is no symbol at all. Promising a QR that never arrives would be the same
    // class of lie as `{sent: true}`.
    doc.blocks.push_back(
        profile.completion == CompletionMechanism::GsParenH
            ? labelRow("QR", "the trailer QR below carries this ticket's own "
                             "verification token")
            : labelRow("QR", "no verification identifier on this path: the token is the "
                             "GS ( H echo, and this profile has no GS ( H"));
  }

  const std::string drawer =
      summary.drawer_present
          ? (std::string(to_string(summary.drawer_standard)) + "  " +
             (summary.drawer_voltage == 0 ? std::string("voltage not documented")
                                          : std::to_string(summary.drawer_voltage) + " V") +
             " - electrical " + to_string(summary.drawer_electrical_provenance) +
             ", commands " + to_string(summary.drawer_commands_provenance) +
             (summary.drawer_kickable ? ", kickable" : ", NOT kickable"))
          : std::string("no drawer port established on this profile");
  doc.blocks.push_back(labelRow("DRAWER", drawer));

  // The mechanism leads the line on purpose: it is the one token an operator has to be
  // able to read off the paper whole, and leading with it keeps it out of a line break
  // at every media width this renderer supports.
  doc.blocks.push_back(labelRow(
      "FENCE", summary.method +
                   " is confirming this ticket; the terminal result below proves the "
                   "mechanism live, on this unit, over this interface path"));

  if (!profile.drivable()) {
    doc.blocks.push_back(labelRow(
        "REFUSED", std::string("this profile is driven in ") + to_string(profile.language) +
                       ", which this engine does not speak: the job below is refused "
                       "before a byte reaches the link"));
  }

  if (!degradations.empty()) {
    doc.blocks.push_back(divider());
    doc.blocks.push_back(textBlock("DECLARED DEGRADATIONS", "centered"));
    for (const std::string& line : degradations) {
      doc.blocks.push_back(textBlock("! " + line, ""));
    }
  }

  doc.blocks.push_back(feed(1));
  return doc;
}

}  // namespace

SelfTestResult Printer::selfTest(const SelfTestOptions& options) {
  SelfTestResult out;

  // 1. Identity: what is stored, or a fresh interrogation when asked for one. The probe
  //    is the driver's own, on this printer's worker thread — never a second engine.
  std::optional<CapabilityFindings> findings =
      options.refresh_identity ? probeNow(options.probe_prints_test_lines) : this->findings();
  const CapabilityProfile profile = this->profile();

  IdentityAssessment assessment;
  if (findings.has_value() && findings->reported.answered()) {
    assessment = identify(findings->reported, IdentityHints{}, findings->behaviour());
  } else {
    // Nothing has ever asked this device. Report the profile's own identity — which is
    // what the caller configured, not what the device claims — and say so through a
    // confidence of zero and trusted false.
    assessment.vendor_guess =
        profile.identity.vendor.empty() ? "Unknown" : profile.identity.vendor;
    assessment.reported_model = profile.identity.model;
    assessment.firmware = profile.identity.firmware;
    assessment.serial = profile.identity.serial;
    assessment.profile_guess = profile.name;
    assessment.signals.push_back(
        "no identification has been performed on this device: the identity above is the "
        "configured profile's, not the device's own");
  }

  out.detection = detail::summarize(id(), profile, assessment);
  out.detection.identity_fresh = options.refresh_identity;

  // 2. Layout. The renderer's media facts come from the same capability profile the
  //    engine drives, so what is laid out is what this printer can hold.
  dsl::RenderOptions render_options;
  render_options.profile = dsl::RenderProfile::from(profile, widthDots());
  render_options.profile.code_page = options.code_page;
  render_options.emit_code_page = true;
  out.detection.chars_per_line =
      render_options.profile.charsPerLine(dsl::ResolvedStyle{});

  const uint64_t stamp_ms = unixMillis();

  // Pass one establishes what the profile cannot draw; pass two prints it.
  const dsl::LayoutOutput probe_layout = dsl::layoutDocument(
      buildTicket(out.detection, options, profile, stamp_ms, {}), render_options);
  std::vector<std::string> degradations;
  for (const dsl::ReportEntry& entry : probe_layout.report.entries) {
    degradations.push_back(degradationLine(entry));
  }

  const Document ticket =
      buildTicket(out.detection, options, profile, stamp_ms, degradations);
  const dsl::RenderOutput rendered = dsl::render(ticket, render_options);
  const dsl::TextPreview preview = dsl::renderText(ticket, render_options);

  out.detection.degradations = degradations;
  out.ticket_lines = preview.lines;

  // 3. Submit. An ordinary job: same worker, same preflight, same fence, same grading.
  JobOptions job_options;
  job_options.key = options.key.empty()
                        ? "selftest-" + std::to_string(stamp_ms)
                        : options.key;
  job_options.print_verification_id = options.print_verification_id;
  job_options.timeout_ms = options.timeout_ms;
  out.key = job_options.key;

  std::shared_ptr<PrintJob> job =
      print(Payload::document(rendered.ops), job_options);
  out.job = job;
  if (!job) {
    out.result = JobResult::failed(FailureReason::Unknown);
    return out;
  }
  out.result = job->result();
  out.print_token = job->printToken();
  return out;
}

}  // namespace pd
