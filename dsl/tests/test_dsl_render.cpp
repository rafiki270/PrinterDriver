#include <string>
#include <vector>

#include "printerdriver/dsl/render.hpp"
#include "printerdriver/escpos_encoder.hpp"
#include "test_harness.hpp"

using namespace pd::dsl;

namespace {

RenderOptions at(uint32_t width_dots) {
  RenderOptions options;
  options.profile = RenderProfile::forWidth(width_dots);
  return options;
}

std::vector<std::string> previewOf(const char* source, uint32_t width_dots) {
  return renderText(parseDocument(source), at(width_dots)).lines;
}

const char kItemRow[] =
    R"({"blocks":[{"columns":[)"
    R"({"content":"2x Pilsner Urquell tankove cerstve","width":"flex"},)"
    R"({"content":"129.00","width":{"chars":8},"align":"right"}]}]})";

}  // namespace

PD_TEST(profile_widths_are_the_documented_character_counts) {
  // Font A is 12 dots wide at 203 dpi: 576/12 = 48 and 384/12 = 32.
  const ResolvedStyle plain;
  CHECK_EQ(RenderProfile::forWidth(576).charsPerLine(plain), 48u);
  CHECK_EQ(RenderProfile::forWidth(384).charsPerLine(plain), 32u);
  CHECK_EQ(RenderProfile::forWidth(504).charsPerLine(plain), 42u);

  ResolvedStyle wide;
  wide.width_scale = 2;
  CHECK_EQ(RenderProfile::forWidth(576).charsPerLine(wide), 24u);

  ResolvedStyle font_b;
  font_b.font = "printerB";
  CHECK_EQ(RenderProfile::forWidth(576).charsPerLine(font_b), 64u);

  ResolvedStyle margined;
  margined.margin_left_dots = 24;
  margined.margin_right_dots = 24;
  CHECK_EQ(RenderProfile::forWidth(576).charsPerLine(margined), 44u);

  // The media facts come from the capability profile.
  const RenderProfile from_profile = RenderProfile::from(pd::xp_s260m());
  CHECK_EQ(from_profile.width_dots, pd::xp_s260m().media.printable_width_dots);
  CHECK_EQ(from_profile.dpi, static_cast<uint16_t>(203));
}

PD_TEST(column_wrap_golden_at_48_characters) {
  const std::vector<std::string> lines = previewOf(kItemRow, 576);
  CHECK_EQ(lines.size(), static_cast<size_t>(1));
  // 40 columns of flex + 8 of fixed, right-aligned amount.
  CHECK_EQ(lines.at(0),
           std::string("2x Pilsner Urquell tankove cerstve" "      " "  129.00"));
  CHECK_EQ(lines.at(0).size(), static_cast<size_t>(48));
}

PD_TEST(column_wrap_golden_at_32_characters) {
  const std::vector<std::string> lines = previewOf(kItemRow, 384);
  // The flex cell is 24 columns wide, so the description takes two rows and the
  // amount cell blanks out under itself.
  CHECK_EQ(lines.size(), static_cast<size_t>(2));
  CHECK_EQ(lines.at(0), std::string("2x Pilsner Urquell" "      " "  129.00"));
  CHECK_EQ(lines.at(1), std::string("tankove cerstve" "         " "        "));
  CHECK_EQ(lines.at(0).size(), static_cast<size_t>(32));
  CHECK_EQ(lines.at(1).size(), static_cast<size_t>(32));
}

PD_TEST(per_cell_wrap_modes) {
  const char* source =
      R"({"blocks":[{"columns":[)"
      R"({"content":"aaaa bbbb cccc","width":{"chars":10},"wrap":"clip"},)"
      R"({"content":"aaaa bbbb cccc","width":{"chars":10},"wrap":"ellipsis"},)"
      R"({"content":"aaaa bbbb cccc","width":{"chars":10},"wrap":"char"}]}]})";
  const std::vector<std::string> lines = previewOf(source, 576);
  CHECK_EQ(lines.size(), static_cast<size_t>(2));
  CHECK_EQ(lines.at(0).substr(0, 30), std::string("aaaa bbbb aaaa bb...aaaa bbbb "));
  CHECK_EQ(lines.at(1).substr(0, 30), std::string("                    cccc      "));
}

PD_TEST(flex_weights_and_overwide_fixed_columns) {
  const char* weighted =
      R"({"blocks":[{"columns":[)"
      R"({"content":"L","width":{"flex":1}},)"
      R"({"content":"R","width":{"flex":3},"align":"right"}]}]})";
  const std::vector<std::string> lines = previewOf(weighted, 576);
  CHECK_EQ(lines.size(), static_cast<size_t>(1));
  CHECK_EQ(lines.at(0).find('L'), static_cast<size_t>(0));
  CHECK_EQ(lines.at(0).find('R'), static_cast<size_t>(47));  // 12 + 36 columns

  // Fixed widths that do not fit are clipped, and the clipping is declared.
  const char* too_wide =
      R"({"blocks":[{"columns":[)"
      R"({"content":"a","width":{"chars":40}},)"
      R"({"content":"b","width":{"chars":40}}]}]})";
  const TextPreview preview = renderText(parseDocument(too_wide), at(576));
  CHECK_EQ(preview.lines.at(0).size(), static_cast<size_t>(48));
  CHECK_EQ(preview.report.count(ReportKind::Truncated), static_cast<size_t>(1));
}

PD_TEST(dividers_fill_the_line) {
  const char* source =
      R"({"blocks":[{"divider":"solid"},{"divider":"dashed"},{"divider":"double"},)"
      R"({"divider":"*"},{"divider":{"style":"char","char":"=","thicknessDots":4}}]})";
  const TextPreview preview = renderText(parseDocument(source), at(576));
  CHECK_EQ(preview.lines.size(), static_cast<size_t>(5));
  CHECK_EQ(preview.lines[0], std::string(48, '-'));
  CHECK_EQ(preview.lines[1], std::string("- - - - - - - - - - - - - - - - - - - - - - - - "));
  CHECK_EQ(preview.lines[2], std::string(48, '='));
  CHECK_EQ(preview.lines[3], std::string(48, '*'));
  CHECK_EQ(preview.lines[4], std::string(48, '='));
  // A dot-height rule is a raster feature; the character rule that replaced it is named.
  CHECK_EQ(preview.report.count(ReportKind::UnsupportedStyle), static_cast<size_t>(1));
}

PD_TEST(text_wraps_and_aligns_to_the_media) {
  const char* source =
      R"({"styles":{"h1":{"widthScale":2,"align":"center"}},)"
      R"("blocks":[{"text":"THE VERY LONG RESTAURANT NAME","style":"h1"},)"
      R"({"text":"line one\nline two"}]})";
  const std::vector<std::string> lines = previewOf(source, 576);
  // Double width halves the line to 24 columns.
  CHECK_EQ(lines.size(), static_cast<size_t>(4));
  CHECK_EQ(lines[0], std::string("THE VERY LONG RESTAURANT"));
  CHECK_EQ(lines[1], std::string("          NAME          "));
  CHECK_EQ(lines[2].substr(0, 8), std::string("line one"));
  CHECK_EQ(lines[3].substr(0, 8), std::string("line two"));
}

PD_TEST(leading_indent_survives_word_wrap) {
  // docs/receipt-dsl.md nests modifiers with "  + {{name}}"; the indent is layout, not
  // a separator, and every continuation line keeps it.
  const std::vector<std::string> lines = previewOf(
      R"({"blocks":[{"text":"  + extra bread with a very long modifier name here"}]})",
      384);
  CHECK_EQ(lines.size(), static_cast<size_t>(2));
  CHECK_EQ(lines[0], std::string("  + extra bread with a very long"));
  CHECK_EQ(lines[1], std::string("  modifier name here            "));
}

PD_TEST(style_resolution_reaches_the_wire) {
  const char* source =
      R"({"styles":{"h1":{"bold":true,"widthScale":2,"heightScale":2,"align":"center"}},)"
      R"("blocks":[{"text":"HELLO","style":"h1"},{"text":"plain"}]})";
  const RenderOutput output = render(parseDocument(source), at(576));
  CHECK_BYTES(output.bytes(),
              0x1B, 0x61, 0x01,              // ESC a 1 — centre
              0x1B, 0x45, 0x01,              // ESC E 1 — bold on
              0x1D, 0x21, 0x11,              // GS ! 0x11 — 2x2
              'H', 'E', 'L', 'L', 'O', 0x0A,
              0x1B, 0x61, 0x00,              // back to the default style
              0x1B, 0x45, 0x00,
              0x1D, 0x21, 0x00,
              'p', 'l', 'a', 'i', 'n', 0x0A);
  CHECK(output.report.empty());
  CHECK(!output.requested_cut.has_value());
  CHECK(!output.requested_margins.any());
}

PD_TEST(styles_without_an_encoder_method_are_emitted_as_commands) {
  const char* source =
      R"({"blocks":[{"text":"x","style":{"font":"printerB","inverse":true,)"
      R"("upsideDown":true,"underline":"double","lineSpacingDots":40,)"
      R"("letterSpacingDots":2,"marginLeftDots":24,"marginRightDots":24}}]})";
  const RenderOutput output = render(parseDocument(source), at(576));
  CHECK_BYTES(output.bytes(),
              0x1B, 0x4D, 0x01,              // ESC M 1 — font B
              0x1D, 0x42, 0x01,              // GS B 1 — inverse
              0x1B, 0x7B, 0x01,              // ESC { 1 — upside down
              0x1B, 0x33, 0x28,              // ESC 3 40 — line spacing
              0x1B, 0x20, 0x02,              // ESC SP 2 — letter spacing
              0x1D, 0x4C, 0x18, 0x00,        // GS L 24 — left margin
              0x1D, 0x57, 0x10, 0x02,        // GS W 528 — print area, both margins off
              0x1B, 0x2D, 0x02,              // ESC - 2 — double underline
              'x', 0x0A);
}

PD_TEST(italic_and_raster_fonts_degrade_declared) {
  // docs/receipt-dsl.md: italic on the hardware path → bold-or-plain + degraded=true.
  const char* source =
      R"({"blocks":[{"text":"a","style":{"italic":true}},)"
      R"({"text":"b","style":{"font":"Inter-600"}},)"
      R"({"text":"c","style":{"rotate90":true}}]})";
  const RenderOutput output = render(parseDocument(source), at(576));
  CHECK_EQ(output.report.count(ReportKind::UnsupportedStyle), static_cast<size_t>(3));
  CHECK_EQ(output.report.entries[0].requested, std::string("italic"));
  CHECK_EQ(output.report.entries[0].delivered, std::string("bold"));
  CHECK_EQ(output.report.entries[1].requested, std::string("font:Inter-600"));
  CHECK_EQ(output.report.entries[1].delivered, std::string("printerA"));
  CHECK_EQ(output.report.entries[2].requested, std::string("rotate90"));
  // The italic line still printed, in bold.
  CHECK_BYTES(output.bytes(), 0x1B, 0x45, 0x01, 'a', 0x0A, 0x1B, 0x45, 0x00, 'b', 0x0A,
              'c', 0x0A);
}

PD_TEST(qr_feed_cut_and_drawer_reach_the_encoder) {
  const char* source =
      R"({"blocks":[{"qr":"7F3A-92C1","size":6,"ec":"M","align":"center"},)"
      R"({"feed":2},{"feed":{"dots":48}},{"cut":"partial"},)"
      R"({"drawerKick":{"pin":0,"pulse":25}},{"text":"tail"}]})";
  const RenderOutput output = render(parseDocument(source), at(576));

  pd::escpos::Encoder expected;
  expected.align(pd::escpos::Alignment::Center)
      .qr("7F3A-92C1", 6, pd::escpos::QrErrorCorrection::M)
      .feedLines(2)
      .feedDots(48)
      // The blade clearance, fed immediately before every cut this renderer emits —
      // the same max(profile, caller) rule the engine applies to the trailing cut.
      .feedDots(RenderProfile::forWidth(576).head_to_cutter_feed_dots)
      .cut(pd::escpos::CutMode::Partial)
      .kickCashDrawer(0, 25, 25)
      .align(pd::escpos::Alignment::Left)
      .line("tail");
  CHECK(output.bytes() == expected.bytes());
  CHECK(output.report.empty());
}

PD_TEST(a_cut_as_the_final_block_is_noted_not_dropped) {
  const RenderOutput output =
      render(parseDocument(R"({"blocks":[{"text":"a"},{"cut":"full"}]})"), at(576));
  CHECK_EQ(output.report.count(ReportKind::Note), static_cast<size_t>(1));
  const pd::escpos::Bytes& bytes = output.bytes();
  CHECK_EQ(bytes[bytes.size() - 3], static_cast<uint8_t>(0x1D));  // GS V 0 — full cut
  CHECK_EQ(bytes[bytes.size() - 2], static_cast<uint8_t>(0x56));
  CHECK_EQ(bytes[bytes.size() - 1], static_cast<uint8_t>(0x00));
}

PD_TEST(an_ean13_block_reaches_the_wire_as_gs_k) {
  const char* source =
      R"({"blocks":[{"text":"before"},)"
      R"({"barcode":"5901234123457","symbology":"ean13","hri":"below",)"
      R"("height":64,"moduleWidth":2},)"
      R"({"text":"after"}]})";
  const RenderOutput output = render(parseDocument(source), at(576));
  CHECK(output.report.empty());
  CHECK_BYTES(output.bytes(), 'b', 'e', 'f', 'o', 'r', 'e', 0x0A,
              0x1D, 0x68, 0x40,        // GS h 64
              0x1D, 0x77, 0x02,        // GS w 2
              0x1D, 0x48, 0x02,        // GS H 2 — HRI below
              0x1D, 0x6B, 0x43, 0x0D,  // GS k 67 13 — EAN-13
              '5', '9', '0', '1', '2', '3', '4', '1', '2', '3', '4', '5', '7',
              'a', 'f', 't', 'e', 'r', 0x0A);
}

PD_TEST(a_code128_block_reaches_the_wire_in_the_optimised_subset) {
  const char* source =
      R"({"blocks":[{"barcode":"ORDER-7F3A","symbology":"code128","align":"center"}]})";
  const RenderOutput output = render(parseDocument(source), at(576));
  CHECK(output.report.empty());
  CHECK_BYTES(output.bytes(), 0x1B, 0x61, 0x01,  // ESC a 1 — centred
              0x1D, 0x68, 0x40, 0x1D, 0x77, 0x02, 0x1D, 0x48, 0x00,
              0x1D, 0x6B, 0x49, 0x0C,  // GS k 73 12 — Code 128
              0x7B, 0x42, 'O', 'R', 'D', 'E', 'R', '-', '7', 'F', '3', 'A');
}

PD_TEST(unsupported_symbologies_are_declared_missing_never_silently_dropped) {
  const char* source =
      R"({"blocks":[{"text":"before"},)"
      R"({"barcode":"5901234123457","symbology":"pdf417","hri":"below"},)"
      R"({"text":"after"}]})";
  const RenderOutput output = render(parseDocument(source), at(576));
  CHECK_EQ(output.report.count(ReportKind::UnsupportedBlock), static_cast<size_t>(1));
  const ReportEntry& entry = output.report.entries.front();
  CHECK_EQ(entry.block, std::string("blocks[1]"));
  CHECK_EQ(entry.requested, std::string("barcode:pdf417 \"5901234123457\""));
  CHECK_EQ(entry.delivered, std::string("omitted"));
  CHECK(entry.path == RenderPath::NotRendered);
  // The surrounding receipt is unaffected.
  CHECK_BYTES(output.bytes(), 'b', 'e', 'f', 'o', 'r', 'e', 0x0A, 'a', 'f', 't', 'e', 'r',
              0x0A);
}

PD_TEST(barcode_data_the_symbology_refuses_is_declared_not_half_drawn) {
  // A wrong EAN check digit would otherwise print a scannable symbol for the wrong
  // article: nothing is emitted, and the reason says which digit was expected.
  const RenderOutput output = render(
      parseDocument(R"({"blocks":[{"barcode":"5901234123450","symbology":"ean13"}]})"),
      at(576));
  CHECK_EQ(output.report.count(ReportKind::UnsupportedBlock), static_cast<size_t>(1));
  CHECK(output.report.entries.front().detail.find("expected 7") != std::string::npos);
  CHECK(output.bytes().empty());

  // A profile with no barcode support declares the omission the same way.
  RenderOptions no_barcodes = at(576);
  no_barcodes.profile.barcodes = false;
  const RenderOutput off = render(
      parseDocument(R"({"blocks":[{"barcode":"96385074","symbology":"ean8"}]})"),
      no_barcodes);
  CHECK_EQ(off.report.count(ReportKind::UnsupportedBlock), static_cast<size_t>(1));
  CHECK(off.bytes().empty());
}

PD_TEST(a_mid_document_cut_feeds_the_blade_clearance_first) {
  // kimix finding: the trailing cut cleared the head-to-blade gap and a mid-document
  // cut did not, so a two-ticket document clipped the bottom of every ticket but the
  // last. Same max() rule, same unconditional floor.
  RenderOptions options = at(576);
  options.profile.head_to_cutter_feed_dots = 150;
  const RenderOutput output = render(
      parseDocument(R"({"blocks":[{"text":"a"},{"cut":"partial"},{"text":"b"}]})"),
      options);
  CHECK_BYTES(output.bytes(), 'a', 0x0A,
              0x1B, 0x4A, 0x96,  // ESC J 150 — the clearance
              0x1D, 0x56, 0x01,  // GS V 1 — partial cut
              'b', 0x0A);

  // A caller asking for more whitespace gets it; asking for less is refused, because
  // the clearance floor is what stops the blade from taking the last content line.
  options.cut_clearance_dots = 200;
  const RenderOutput wider = render(
      parseDocument(R"({"blocks":[{"cut":"partial"}]})"), options);
  CHECK_BYTES(wider.bytes(), 0x1B, 0x4A, 0xC8, 0x1D, 0x56, 0x01);

  options.cut_clearance_dots = 8;
  const RenderOutput floored = render(
      parseDocument(R"({"blocks":[{"cut":"partial"}]})"), options);
  CHECK_BYTES(floored.bytes(), 0x1B, 0x4A, 0x96, 0x1D, 0x56, 0x01);
}

PD_TEST(the_blade_clearance_comes_from_the_capability_profile) {
  pd::CapabilityProfile capability;
  capability.media.head_to_cutter_feed_dots = 96;
  const RenderProfile profile = RenderProfile::from(capability, 576);
  CHECK_EQ(profile.head_to_cutter_feed_dots, static_cast<uint16_t>(96));
}

PD_TEST(images_render_through_the_encoder_raster_path) {
  std::vector<uint8_t> gray(64 * 8, 0);
  RenderOptions options = at(576);
  options.images.push_back(ImageAsset{"logo", gray, 64, 8});

  const RenderOutput output = render(
      parseDocument(R"({"blocks":[{"image":"logo","width":{"percent":50},"align":"center"}]})"),
      options);
  pd::escpos::Encoder expected;
  expected.align(pd::escpos::Alignment::Center)
      .rasterGrayscale(gray.data(), 64, 8, 288, pd::escpos::Binarization::FloydSteinberg,
                       128, pd::escpos::RasterScale::Normal, 1024);
  CHECK(output.bytes() == expected.bytes());
  CHECK(output.report.empty());

  // A reference nobody supplied is declared, not skipped in silence.
  const RenderOutput missing =
      render(parseDocument(R"({"blocks":[{"image":"nope"}]})"), at(576));
  CHECK_EQ(missing.report.count(ReportKind::MissingImage), static_cast<size_t>(1));
  CHECK(missing.bytes().empty());
}

PD_TEST(raw_blocks_pass_through_and_framing_risks_are_named) {
  // "raw" here is base64 for 1D 56 01 — a cut the core is supposed to own.
  const RenderOutput risky =
      render(parseDocument(R"({"blocks":[{"raw":"HVYB"}]})"), at(576));
  CHECK_EQ(risky.report.count(ReportKind::RawFramingRisk), static_cast<size_t>(1));
  CHECK_BYTES(risky.bytes(), 0x1D, 0x56, 0x01);

  const RenderOutput plain =
      render(parseDocument(R"({"blocks":[{"raw":"QUI="}]})"), at(576));
  CHECK(plain.report.empty());
  CHECK_BYTES(plain.bytes(), 'A', 'B');
}

PD_TEST(unbound_template_blocks_are_declared) {
  const RenderOutput output = render(
      parseDocument(R"({"blocks":[{"each":"items","block":{"text":"{{name}}"}}]})"),
      at(576));
  CHECK_EQ(output.report.count(ReportKind::UnsupportedBlock), static_cast<size_t>(1));
  CHECK_EQ(output.report.entries.front().requested, std::string("each:items"));
  CHECK(output.bytes().empty());
}

PD_TEST(meta_cut_and_margins_are_returned_not_applied) {
  // docs/receipt-dsl.md: cut and margins are the caller's to apply — the renderer only
  // reports what the document asked for, and never touches the engine's framing.
  const RenderOutput off = render(
      parseDocument(R"({"meta":{"cut":false},"blocks":[{"text":"a"}]})"), at(576));
  CHECK(off.requested_cut.has_value());
  CHECK(*off.requested_cut == CutRequest::None);
  CHECK_BYTES(off.bytes(), 'a', 0x0A);  // no GS V anywhere

  const RenderOutput full = render(
      parseDocument(R"({"meta":{"cut":"full"},"blocks":[]})"), at(576));
  CHECK(*full.requested_cut == CutRequest::Full);

  const RenderOutput dots = render(
      parseDocument(R"({"meta":{"margins":{"topDots":40,"bottomDots":160}},"blocks":[]})"),
      at(576));
  CHECK_EQ(*dots.requested_margins.top_dots, 40u);
  CHECK_EQ(*dots.requested_margins.bottom_dots, 160u);

  // Millimetres convert at the profile's dpi: 203 dpi is 8 dots/mm.
  const RenderOutput mm = render(
      parseDocument(R"({"meta":{"margins":{"topMm":5,"bottomMm":20}},"blocks":[]})"),
      at(576));
  CHECK_EQ(*mm.requested_margins.top_dots, 40u);
  CHECK_EQ(*mm.requested_margins.bottom_dots, 160u);
}

PD_TEST(the_code_page_comes_from_the_profile) {
  RenderOptions options = at(576);
  options.profile.code_page = pd::escpos::CodePage::PC852;
  const RenderOutput output =
      render(parseDocument(R"({"blocks":[{"text":"cena"}]})"), options);
  CHECK_BYTES(output.bytes(), 0x1B, 0x74, 0x12, 'c', 'e', 'n', 'a', 0x0A);
  CHECK(output.codePage() == pd::escpos::CodePage::PC852);

  options.emit_code_page = false;
  options.emit_initialize = true;
  const RenderOutput bare =
      render(parseDocument(R"({"blocks":[{"text":"x"}]})"), options);
  CHECK_BYTES(bare.bytes(), 0x1B, 0x40, 'x', 0x0A);
}

PD_TEST(the_preview_and_the_bytes_come_from_one_layout_pass) {
  const char* source =
      R"({"blocks":[{"text":"header"},{"columns":[{"content":"a","width":"flex"},)"
      R"({"content":"1","width":{"chars":4},"align":"right"}]},)"
      R"({"qr":"x","size":4},{"feed":2},{"cut":"partial"},{"raw":"QQ=="}]})";
  const TextPreview preview = renderText(parseDocument(source), at(384));
  const LayoutOutput laid = layoutDocument(parseDocument(source), at(384));
  CHECK_EQ(laid.ops.size(), static_cast<size_t>(6));
  CHECK_EQ(preview.lines.size(), static_cast<size_t>(7));  // feed 2 becomes two blanks
  CHECK_EQ(preview.lines[0].size(), static_cast<size_t>(32));
  CHECK_EQ(preview.lines[1], std::string("a") + std::string(27, ' ') + "   1");
  CHECK_EQ(preview.lines[2], std::string("[qr 4/M \"x\"]"));
  CHECK_EQ(preview.lines[3], std::string());
  CHECK_EQ(preview.lines[5], std::string("[cut partial after 120 dots clearance]"));
  CHECK_EQ(preview.lines[6], std::string("[raw 1 bytes]"));
  CHECK(preview.text().find("[qr 4/M \"x\"]\n") != std::string::npos);
}

PD_TEST(profiles_without_a_feature_declare_the_omission) {
  RenderOptions options = at(576);
  options.profile.qr = false;
  options.profile.cutter = false;
  options.profile.images = false;
  options.profile.drawer_kick = false;
  const RenderOutput output = render(
      parseDocument(R"({"blocks":[{"qr":"a"},{"cut":"full"},{"image":"l"},)"
                    R"({"drawerKick":true}]})"),
      options);
  CHECK_EQ(output.report.count(ReportKind::UnsupportedBlock), static_cast<size_t>(4));
  CHECK(output.bytes().empty());
}

PD_TEST(render_clamps_absurd_feed_dots_and_declares_it) {
  // kimix adversarial-review finding: {"feed":{"dots":INT32_MAX}} made the text
  // preview allocate ~90 million lines. The layout clamp bounds both paths.
  const char* doc_json = R"({"v":1,"blocks":[{"text":"x"},{"feed":{"dots":2147483647}}]})";
  pd::dsl::Document doc = pd::dsl::parseDocument(pd::dsl::parseJson(doc_json));
  pd::dsl::RenderOptions options;
  pd::dsl::RenderOutput out = pd::dsl::render(doc, options);
  bool declared = false;
  for (const auto& entry : out.report.entries) {
    if (entry.requested.find("feed of 2147483647") != std::string::npos) {
      declared = true;
      CHECK_EQ(entry.delivered, std::string("clamped to 65535"));
    }
  }
  CHECK(declared);
  const std::string preview = pd::dsl::renderText(doc, options).text();
  CHECK(preview.size() < 1000000);  // bounded, not ~90M lines
}
