#include <string>
#include <vector>

#include "printerdriver/dsl/document.hpp"
#include "test_harness.hpp"

using namespace pd::dsl;

namespace {

// docs/receipt-dsl.md "JSON canonical form (what travels and gets stored)".
const char kCanonical[] = R"JSON(
{ "v": 1,
  "styles": { "h1": { "bold": true, "widthScale": 2, "heightScale": 2, "align": "center" } },
  "blocks": [
    { "text": "MY RESTAURANT", "style": "h1" },
    { "columns": [ { "content": "2× Pilsner", "width": "flex" },
                   { "content": "9.00", "width": { "chars": 8 }, "align": "right" } ] },
    { "qr": "7F3A-92C1", "size": 6, "ec": "M", "align": "center" } ] }
)JSON";

// docs/receipt-dsl.md "Templates + parameter models".
const char kTemplate[] = R"JSON(
{ "v": 1, "template": true,
  "styles": { "h1": { "bold": true, "widthScale": 2, "align": "center" } },
  "blocks": [
    { "text": "{{venue.name}}", "style": "h1" },
    { "text": "Order {{order.id}} · Table {{order.table}}" },
    { "divider": "dashed" },
    { "each": "order.items", "block":
      { "columns": [ { "content": "{{qty}}× {{name}}", "width": "flex" },
                     { "content": "{{price}}", "width": { "chars": 8 }, "align": "right" } ] } },
    { "divider": "solid" },
    { "columns": [ { "content": "TOTAL", "width": "flex", "style": "total" },
                   { "content": "{{order.total}}", "width": { "chars": 8 }, "align": "right", "style": "total" } ] },
    { "if": "order.note", "block": { "text": "NOTE: {{order.note}}", "style": "bold" } },
    { "qr": "{{order.id}}", "size": 6, "align": "center" } ] }
)JSON";

// docs/receipt-dsl.md "Repeating groups (multi-line, nested) and value formatters".
const char kNestedGroup[] = R"JSON(
{ "v": 1, "template": true,
  "meta": { "locale": "cs-CZ", "currency": "CZK", "tz": "Europe/Prague" },
  "blocks": [
    { "each": "order.items", "blocks": [
        { "text": "{{title}}", "style": "itemTitle" },
        { "if": "subtitle", "block": { "text": "{{subtitle}}", "style": "muted" } },
        { "each": "modifiers", "blocks": [ { "text": "  + {{name}}", "style": "muted" } ] },
        { "columns": [ { "content": "{{qty}} × {{unitPrice|number:2}}", "width": "flex" },
                       { "content": "{{total|money}}", "width": { "chars": 10 }, "align": "right" } ] },
        { "feed": 1 } ] } ] }
)JSON";

}  // namespace

PD_TEST(parses_the_spec_canonical_document) {
  const Document document = parseDocument(kCanonical);
  CHECK_EQ(document.version, 1);
  CHECK(!document.is_template);
  CHECK_EQ(document.styles.size(), static_cast<size_t>(1));
  CHECK_EQ(document.styles.front().first, std::string("h1"));
  CHECK(*document.styles.front().second.bold);
  CHECK_EQ(*document.styles.front().second.width_scale, 2);
  CHECK_EQ(document.blocks.size(), static_cast<size_t>(3));

  const Block& heading = document.blocks[0];
  CHECK(heading.kind == Block::Kind::Text);
  CHECK_EQ(heading.content, std::string("MY RESTAURANT"));
  CHECK_EQ(*heading.style.name, std::string("h1"));

  const Block& row = document.blocks[1];
  CHECK(row.kind == Block::Kind::Columns);
  CHECK_EQ(row.cells.size(), static_cast<size_t>(2));
  CHECK(row.cells[0].width.unit == CellWidth::Unit::Flex);
  CHECK(row.cells[1].width.unit == CellWidth::Unit::Chars);
  CHECK_EQ(row.cells[1].width.value, 8);
  CHECK(*row.cells[1].align == Align::Right);
  CHECK(!row.cells[0].align.has_value());  // absent stays absent

  const Block& code = document.blocks[2];
  CHECK(code.kind == Block::Kind::Qr);
  CHECK_EQ(code.content, std::string("7F3A-92C1"));
  CHECK_EQ(code.qr_size, 6);
  CHECK(code.qr_ec == QrEc::M);
  CHECK(code.align == Align::Center);
}

PD_TEST(parses_the_spec_template) {
  const Document document = parseDocument(kTemplate);
  CHECK(document.is_template);
  CHECK_EQ(document.blocks.size(), static_cast<size_t>(8));

  CHECK(document.blocks[2].kind == Block::Kind::Divider);
  CHECK(document.blocks[2].divider == DividerKind::Dashed);

  const Block& each = document.blocks[3];
  CHECK(each.kind == Block::Kind::Each);
  CHECK_EQ(each.path, std::string("order.items"));
  CHECK(each.body_is_single);
  CHECK_EQ(each.body.size(), static_cast<size_t>(1));
  CHECK(each.body.front().kind == Block::Kind::Columns);
  CHECK_EQ(each.body.front().cells[0].content, std::string("{{qty}}\xC3\x97 {{name}}"));

  const Block& conditional = document.blocks[6];
  CHECK(conditional.kind == Block::Kind::If);
  CHECK(!conditional.negated);
  CHECK_EQ(conditional.path, std::string("order.note"));
  CHECK_EQ(conditional.body.size(), static_cast<size_t>(1));
}

PD_TEST(parses_the_spec_nested_repeating_group) {
  const Document document = parseDocument(kNestedGroup);
  CHECK_EQ(document.meta.locale, std::string("cs-CZ"));
  CHECK_EQ(document.meta.currency, std::string("CZK"));
  CHECK_EQ(document.meta.tz, std::string("Europe/Prague"));

  const Block& group = document.blocks.at(0);
  CHECK(group.kind == Block::Kind::Each);
  CHECK(!group.body_is_single);
  CHECK_EQ(group.body.size(), static_cast<size_t>(5));
  CHECK(group.body[1].kind == Block::Kind::If);
  CHECK(group.body[2].kind == Block::Kind::Each);
  CHECK_EQ(group.body[2].path, std::string("modifiers"));
  CHECK(group.body[3].kind == Block::Kind::Columns);
  CHECK_EQ(group.body[3].cells[1].content, std::string("{{total|money}}"));
  CHECK(group.body[4].kind == Block::Kind::Feed);
  CHECK_EQ(group.body[4].feed_lines, 1);
}

PD_TEST(serialization_round_trips) {
  for (const char* source : {kCanonical, kTemplate, kNestedGroup}) {
    const Document once = parseDocument(source);
    const std::string first = serializeDocument(once);
    const std::string second = serializeDocument(parseDocument(first));
    CHECK_EQ(second, first);
  }

  // And the canonical form comes back as canonical JSON, key for key.
  const Document document = parseDocument(kCanonical);
  CHECK_EQ(serializeDocument(document),
           std::string(
               R"({"v":1,"styles":{"h1":{"widthScale":2,"heightScale":2,"bold":true,)"
               R"("align":"center"}},"blocks":[{"text":"MY RESTAURANT","style":"h1"},)"
               R"({"columns":[{"content":"2× Pilsner","width":"flex"},)"
               R"({"content":"9.00","width":{"chars":8},"align":"right"}]},)"
               R"({"qr":"7F3A-92C1","size":6,"ec":"M","align":"center"}]})"));
}

PD_TEST(unknown_keys_are_reported_not_rejected) {
  std::vector<std::string> warnings;
  const Document document =
      parseDocument(R"({"v":1,"future":true,"blocks":[{"text":"x","nope":1}]})", &warnings);
  CHECK_EQ(document.blocks.size(), static_cast<size_t>(1));
  CHECK_EQ(warnings.size(), static_cast<size_t>(2));
  bool saw_document_key = false;
  bool saw_block_key = false;
  for (const std::string& warning : warnings) {
    saw_document_key = saw_document_key || warning.find("future") != std::string::npos;
    saw_block_key = saw_block_key || warning.find("nope") != std::string::npos;
  }
  CHECK(saw_document_key);
  CHECK(saw_block_key);
}

PD_TEST(structural_errors_throw_before_anything_prints) {
  CHECK_THROWS(parseDocument(R"({"blocks":[{}]})"), DocumentError);
  CHECK_THROWS(parseDocument(R"({"blocks":[{"unknownBlock":1}]})"), DocumentError);
  CHECK_THROWS(parseDocument(R"({"blocks":{}})"), DocumentError);
  CHECK_THROWS(parseDocument(R"({"styles":[]})"), DocumentError);
  CHECK_THROWS(parseDocument(R"({"blocks":[{"columns":[{"width":"wide"}]}]})"),
               DocumentError);
  CHECK_THROWS(parseDocument(R"({"blocks":[{"each":"a"}]})"), DocumentError);
  CHECK_THROWS(parseDocument("[]"), DocumentError);
}

PD_TEST(named_styles_inherit_and_inline_overrides_win) {
  const Document document = parseDocument(R"JSON(
{ "styles": {
    "default": { "font": "printerA", "wrap": "word" },
    "base":    { "bold": true, "align": "center" },
    "h1":      { "extends": "base", "widthScale": 2, "heightScale": 2 },
    "h2":      { "extends": "h1", "widthScale": 1 } },
  "blocks": [] })JSON");

  StyleRef plain;
  const ResolvedStyle base_style = document.resolve(plain);
  CHECK_EQ(base_style.font, std::string("printerA"));
  CHECK(!base_style.bold);
  CHECK(base_style.align == Align::Left);

  StyleRef h1;
  h1.name = "h1";
  const ResolvedStyle heading = document.resolve(h1);
  CHECK(heading.bold);                       // from base, two links up
  CHECK(heading.align == Align::Center);     // from base
  CHECK_EQ(heading.width_scale, 2);
  CHECK_EQ(heading.height_scale, 2);

  StyleRef h2;
  h2.name = "h2";
  const ResolvedStyle sub = document.resolve(h2);
  CHECK(sub.bold);
  CHECK_EQ(sub.width_scale, 1);              // h2 overrides h1
  CHECK_EQ(sub.height_scale, 2);             // and inherits the rest

  // Inline override on top of a named style.
  StyleRef inline_override;
  inline_override.name = "h1";
  inline_override.inline_style.align = Align::Right;
  inline_override.inline_style.underline = Underline::Double;
  const ResolvedStyle overridden = document.resolve(inline_override);
  CHECK(overridden.align == Align::Right);
  CHECK(overridden.underline == Underline::Double);
  CHECK(overridden.bold);
  CHECK_EQ(overridden.width_scale, 2);
}

PD_TEST(unknown_style_names_and_cycles_are_declared_not_thrown) {
  const Document document = parseDocument(R"JSON(
{ "styles": { "a": { "extends": "b", "bold": true }, "b": { "extends": "a" } },
  "blocks": [] })JSON");

  RenderReport report;
  StyleRef missing;
  missing.name = "nope";
  const ResolvedStyle fallback = document.resolve(missing, &report, "blocks[0]");
  CHECK(!fallback.bold);
  CHECK_EQ(report.count(ReportKind::UnknownStyle), static_cast<size_t>(1));
  CHECK_EQ(report.entries.front().requested, std::string("style:nope"));
  CHECK_EQ(report.entries.front().delivered, std::string("default"));

  RenderReport cycle_report;
  StyleRef looping;
  looping.name = "a";
  const ResolvedStyle looped = document.resolve(looping, &cycle_report, "blocks[1]");
  CHECK(looped.bold);  // still resolves what it can
  CHECK_EQ(cycle_report.count(ReportKind::StyleCycle), static_cast<size_t>(1));
}

PD_TEST(scale_out_of_range_is_clamped_and_declared) {
  const Document document =
      parseDocument(R"({"styles":{"huge":{"widthScale":12}},"blocks":[]})");
  RenderReport report;
  StyleRef ref;
  ref.name = "huge";
  const ResolvedStyle style = document.resolve(ref, &report, "blocks[0]");
  CHECK_EQ(style.width_scale, 8);
  CHECK_EQ(report.count(ReportKind::UnsupportedStyle), static_cast<size_t>(1));
}

PD_TEST(meta_carries_cut_and_margins) {
  // docs/receipt-dsl.md "Cut control": a no-cut kitchen-summary template says so itself.
  const Document off = parseDocument(R"({"meta":{"cut":false},"blocks":[]})");
  CHECK(off.meta.cut.has_value());
  CHECK(*off.meta.cut == CutRequest::None);

  const Document full = parseDocument(R"({"meta":{"cut":"full"},"blocks":[]})");
  CHECK(*full.meta.cut == CutRequest::Full);

  const Document on = parseDocument(R"({"meta":{"cut":true},"blocks":[]})");
  CHECK(*on.meta.cut == CutRequest::Profile);

  const Document none = parseDocument(R"({"blocks":[]})");
  CHECK(!none.meta.cut.has_value());  // absent means "the caller and profile decide"

  const Document margins =
      parseDocument(R"({"meta":{"margins":{"topDots":40,"bottomDots":160}},"blocks":[]})");
  CHECK_EQ(*margins.meta.margins.top_dots, 40);
  CHECK_EQ(*margins.meta.margins.bottom_dots, 160);
  CHECK(margins.meta.margins.any());

  const Document mm =
      parseDocument(R"({"meta":{"margins":{"topMm":5,"bottomMm":20}},"blocks":[]})");
  CHECK_EQ(*mm.meta.margins.top_mm, 5.0);
  CHECK_EQ(*mm.meta.margins.bottom_mm, 20.0);
}

PD_TEST(every_block_type_survives_a_round_trip) {
  const char* source =
      R"({"v":1,"blocks":[)"
      R"({"text":"hello","style":{"bold":true,"wrap":"ellipsis"}},)"
      R"({"columns":[{"content":"a","width":{"flex":2}},{"content":"b","width":{"dots":96},"align":"right","wrap":"clip"}]},)"
      R"({"divider":{"style":"char","char":"=","thicknessDots":2}},)"
      R"({"feed":3},)"
      R"({"feed":{"dots":48}},)"
      R"({"cut":"full"},)"
      R"({"drawerKick":{"pin":1,"pulse":50}},)"
      R"({"raw":"G1Q="},)"
      R"({"qr":"payload","size":8,"ec":"H","align":"center"},)"
      R"({"barcode":"12345678","symbology":"ean13","height":80,"moduleWidth":3,"hri":"below","align":"center"},)"
      R"({"image":{"gray":"AAECAw==","width":2,"height":2},"width":{"percent":60},"align":"center","dither":"threshold"},)"
      R"({"unless":"order.paid","blocks":[{"text":"UNPAID"}]})"
      R"(]})";
  const Document document = parseDocument(source);
  CHECK_EQ(document.blocks.size(), static_cast<size_t>(12));
  CHECK(document.blocks[7].kind == Block::Kind::Raw);
  CHECK_EQ(document.blocks[7].raw.size(), static_cast<size_t>(2));
  CHECK_EQ(document.blocks[7].raw[0], static_cast<uint8_t>(0x1B));
  CHECK(document.blocks[9].kind == Block::Kind::Barcode);
  CHECK(document.blocks[9].symbology == Symbology::Ean13);
  CHECK(document.blocks[10].kind == Block::Kind::Image);
  CHECK_EQ(document.blocks[10].image_gray.size(), static_cast<size_t>(4));
  CHECK(document.blocks[11].kind == Block::Kind::If);
  CHECK(document.blocks[11].negated);

  CHECK_EQ(serializeDocument(parseDocument(serializeDocument(document))),
           serializeDocument(document));
}
