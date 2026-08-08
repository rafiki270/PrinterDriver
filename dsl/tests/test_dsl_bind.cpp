#include <string>

#include "printerdriver/dsl/bind.hpp"
#include "test_harness.hpp"

using namespace pd::dsl;

namespace {

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

const char kModel[] = R"JSON(
{ "venue": { "name": "MY RESTAURANT" },
  "order": { "id": "7F3A-92C1", "table": 4, "total": "20.50",
             "items": [ { "qty": 2, "name": "Pilsner", "price": "9.00" },
                        { "qty": 1, "name": "Goulash", "price": "11.50" } ] } }
)JSON";

std::string bound(const char* source, const char* model) {
  const BindOutcome outcome = bind(parseDocument(source), parseJson(model));
  return serializeDocument(outcome.document);
}

}  // namespace

PD_TEST(binds_the_spec_template_exactly) {
  const Document document = parseDocument(kTemplate);
  const BindOutcome outcome = bind(document, parseJson(kModel));

  CHECK(!outcome.document.is_template);
  CHECK_EQ(outcome.document.styles.size(), static_cast<size_t>(1));
  // The `each` produced two rows and the absent note dropped its block:
  // text, text, divider, row, row, divider, total, qr.
  CHECK_EQ(outcome.document.blocks.size(), static_cast<size_t>(8));
  CHECK_EQ(outcome.document.blocks[0].content, std::string("MY RESTAURANT"));
  CHECK_EQ(*outcome.document.blocks[0].style.name, std::string("h1"));
  CHECK_EQ(outcome.document.blocks[1].content,
           std::string("Order 7F3A-92C1 \xC2\xB7 Table 4"));
  CHECK_EQ(outcome.document.blocks[3].cells[0].content, std::string("2\xC3\x97 Pilsner"));
  CHECK_EQ(outcome.document.blocks[3].cells[1].content, std::string("9.00"));
  CHECK_EQ(outcome.document.blocks[4].cells[0].content, std::string("1\xC3\x97 Goulash"));
  CHECK_EQ(outcome.document.blocks[6].cells[1].content, std::string("20.50"));
  CHECK_EQ(outcome.document.blocks[7].content, std::string("7F3A-92C1"));
  CHECK(outcome.document.blocks[7].kind == Block::Kind::Qr);
  // Nothing was missing, so nothing is reported.
  CHECK(outcome.report.empty());

  // With a note, the conditional block appears.
  Json model = parseJson(kModel);
  Json order = *model.find("order");
  order.set("note", Json::string("no onions"));
  model.set("order", order);
  const BindOutcome with_note = bind(document, model);
  CHECK_EQ(with_note.document.blocks.size(), static_cast<size_t>(9));
  CHECK_EQ(with_note.document.blocks[7].content, std::string("NOTE: no onions"));
}

PD_TEST(missing_paths_warn_and_render_empty) {
  // docs/receipt-dsl.md: missing path → declared render-report warning + empty string,
  // never a crash mid-receipt.
  const Document document =
      parseDocument(R"({"blocks":[{"text":"A{{nope}}B{{also.missing}}C"}]})");
  const BindOutcome outcome = bind(document, parseJson(R"({"present":1})"));
  CHECK_EQ(outcome.document.blocks.at(0).content, std::string("ABC"));
  CHECK_EQ(outcome.report.count(ReportKind::MissingPath), static_cast<size_t>(2));
  CHECK_EQ(outcome.report.entries.front().requested, std::string("{{nope}}"));
  CHECK_EQ(outcome.report.entries.front().delivered, std::string());
  CHECK_EQ(outcome.report.entries.front().block, std::string("blocks[0]"));
}

PD_TEST(nested_each_and_conditionals) {
  // docs/receipt-dsl.md "Repeating groups (multi-line, nested)".
  const char* source = R"JSON(
{ "v": 1, "template": true,
  "meta": { "locale": "cs-CZ", "currency": "CZK" },
  "blocks": [
    { "each": "order.items", "blocks": [
        { "text": "{{title}}" },
        { "if": "subtitle", "block": { "text": "{{subtitle}}" } },
        { "each": "modifiers", "blocks": [ { "text": "  + {{name}}" } ] },
        { "columns": [ { "content": "{{qty}} × {{unitPrice|number:2}}", "width": "flex" },
                       { "content": "{{total|money}}", "width": { "chars": 10 }, "align": "right" } ] },
        { "feed": 1 } ] } ] })JSON";
  const char* model = R"JSON(
{ "order": { "items": [
    { "title": "Goulash", "subtitle": "with dumplings", "qty": 2, "unitPrice": 129,
      "total": 258, "modifiers": [ { "name": "extra bread" }, { "name": "no onion" } ] },
    { "title": "Pilsner", "qty": 3, "unitPrice": 49.5, "total": 148.5,
      "modifiers": [] } ] } })JSON";

  const BindOutcome outcome = bind(parseDocument(source), parseJson(model));
  const BlockList& blocks = outcome.document.blocks;
  // Item 1: title, subtitle, two modifiers, row, feed = 6. Item 2: title, row, feed = 3.
  CHECK_EQ(blocks.size(), static_cast<size_t>(9));
  CHECK_EQ(blocks[0].content, std::string("Goulash"));
  CHECK_EQ(blocks[1].content, std::string("with dumplings"));
  CHECK_EQ(blocks[2].content, std::string("  + extra bread"));
  CHECK_EQ(blocks[3].content, std::string("  + no onion"));
  CHECK_EQ(blocks[4].cells[0].content, std::string("2 \xC3\x97 129,00"));
  CHECK_EQ(blocks[4].cells[1].content, std::string("258,00 K\xC4\x8D"));
  CHECK(blocks[5].kind == Block::Kind::Feed);
  CHECK_EQ(blocks[6].content, std::string("Pilsner"));
  CHECK_EQ(blocks[7].cells[0].content, std::string("3 \xC3\x97 49,50"));
  CHECK_EQ(blocks[7].cells[1].content, std::string("148,50 K\xC4\x8D"));

  // The second item has no subtitle (falsy, no warning) and an empty modifier list.
  CHECK_EQ(outcome.report.count(ReportKind::MissingPath), static_cast<size_t>(0));
  CHECK(outcome.report.empty());
}

PD_TEST(unless_is_the_negation_and_missing_is_falsy) {
  const char* source =
      R"({"blocks":[{"if":"a","block":{"text":"IF"}},)"
      R"({"unless":"a","block":{"text":"UNLESS"}}]})";
  const Document document = parseDocument(source);

  const BindOutcome truthy = bind(document, parseJson(R"({"a":"x"})"));
  CHECK_EQ(truthy.document.blocks.size(), static_cast<size_t>(1));
  CHECK_EQ(truthy.document.blocks[0].content, std::string("IF"));

  const BindOutcome falsy = bind(document, parseJson(R"({"a":""})"));
  CHECK_EQ(falsy.document.blocks.size(), static_cast<size_t>(1));
  CHECK_EQ(falsy.document.blocks[0].content, std::string("UNLESS"));

  const BindOutcome absent = bind(document, parseJson("{}"));
  CHECK_EQ(absent.document.blocks.size(), static_cast<size_t>(1));
  CHECK_EQ(absent.document.blocks[0].content, std::string("UNLESS"));
  // An optional field that is simply not there is not worth a warning.
  CHECK(absent.report.empty());

  const BindOutcome zero = bind(document, parseJson(R"({"a":0})"));
  CHECK_EQ(zero.document.blocks[0].content, std::string("UNLESS"));
  const BindOutcome empty_list = bind(document, parseJson(R"({"a":[]})"));
  CHECK_EQ(empty_list.document.blocks[0].content, std::string("UNLESS"));
}

PD_TEST(each_over_a_missing_or_wrong_shaped_path_is_declared) {
  const Document document =
      parseDocument(R"({"blocks":[{"each":"order.items","block":{"text":"x"}}]})");

  const BindOutcome missing = bind(document, parseJson("{}"));
  CHECK(missing.document.blocks.empty());
  CHECK_EQ(missing.report.count(ReportKind::EmptyIteration), static_cast<size_t>(1));

  const BindOutcome wrong = bind(document, parseJson(R"({"order":{"items":"nope"}})"));
  CHECK(wrong.document.blocks.empty());
  CHECK_EQ(wrong.report.count(ReportKind::EmptyIteration), static_cast<size_t>(1));

  const BindOutcome empty = bind(document, parseJson(R"({"order":{"items":[]}})"));
  CHECK(empty.document.blocks.empty());
  CHECK(empty.report.empty());  // an empty list is not a defect
}

PD_TEST(scope_walks_outward_and_exposes_the_element) {
  const char* source = R"JSON(
{ "blocks": [ { "each": "items", "blocks": [
    { "text": "{{@index}} {{name}} at {{venue.name}}" },
    { "text": "{{.}}" } ] } ] })JSON";
  const char* model =
      R"({"venue":{"name":"HQ"},"items":[{"name":"a"},{"name":"b"}]})";
  const BindOutcome outcome = bind(parseDocument(source), parseJson(model));
  CHECK_EQ(outcome.document.blocks.size(), static_cast<size_t>(4));
  CHECK_EQ(outcome.document.blocks[0].content, std::string("0 a at HQ"));
  CHECK_EQ(outcome.document.blocks[1].content, std::string(R"({"name":"a"})"));
  CHECK_EQ(outcome.document.blocks[2].content, std::string("1 b at HQ"));

  // Indexed and dotted paths reach the same value.
  CHECK_EQ(bindString("{{items[1].name}}/{{items.1.name}}", parseJson(model)),
           std::string("b/b"));
  CHECK(lookupPath(parseJson(model), "items[0].name") != nullptr);
  CHECK(lookupPath(parseJson(model), "items[9].name") == nullptr);
  // An index far past any size_t is out of range, not an exception mid-receipt.
  RenderReport report;
  CHECK_EQ(bindString("{{items[99999999999999999999].name}}", parseJson(model), {}, &report),
           std::string());
  CHECK_EQ(report.count(ReportKind::MissingPath), static_cast<size_t>(1));
  // An unterminated bracket is dropped rather than guessed at: the path degrades to
  // "items", which then reports as a value with no receipt representation.
  CHECK(lookupPath(parseJson(model), "items[") != nullptr);
  RenderReport structural;
  CHECK_EQ(bindString("{{items[}}", parseJson(model), {}, &structural),
           std::string(R"([{"name":"a"},{"name":"b"}])"));
  CHECK_EQ(structural.count(ReportKind::UnformattableValue), static_cast<size_t>(1));
}

PD_TEST(substitution_escapes_and_malformed_expressions) {
  Json model;
  model.set("a", Json::string("x"));
  // ESC ! 0x38 selects double-height double-width text. A model value must never
  // be able to smuggle that into a receipt built from a trusted layout.
  model.set("evil", Json::string(std::string("BAD\x1B!8") + "dX"));

  // A backslash escapes the opening braces.
  CHECK_EQ(bindString(R"(\{{a}} = {{a}})", model), std::string("{{a}} = x"));

  // Control bytes in a model value can never reach the printer as commands.
  CHECK_EQ(bindString("{{evil}}", model), std::string("BAD!8dX"));

  RenderReport report;
  CHECK_EQ(bindString("start {{a", model, {}, &report), std::string("start {{a"));
  CHECK_EQ(report.count(ReportKind::MalformedTemplate), static_cast<size_t>(1));

  RenderReport empty_path;
  CHECK_EQ(bindString("a{{}}b", model, {}, &empty_path), std::string("ab"));
  CHECK_EQ(empty_path.count(ReportKind::MalformedTemplate), static_cast<size_t>(1));

  // Whitespace inside the braces is allowed; text with no braces is untouched.
  CHECK_EQ(bindString("{{ a }}", model), std::string("x"));
  CHECK_EQ(bindString("plain text", model), std::string("plain text"));
}

PD_TEST(formatters_chain_and_report_through_binding) {
  const Json model = parseJson(R"({"v":1234.5,"name":"pilsner","when":"2026-08-08T14:05:00"})");
  BindOptions options;
  options.locale = "cs-CZ";
  options.currency = "CZK";

  CHECK_EQ(bindString("{{v|number:2}}", model, options), std::string("1 234,50"));
  CHECK_EQ(bindString("{{v|money}}", model, options), std::string("1 234,50 K\xC4\x8D"));
  CHECK_EQ(bindString("{{when|date:HH:mm}}", model, options), std::string("14:05"));
  CHECK_EQ(bindString("{{name|upper|pad:12:right}}|", model, options),
           std::string("PILSNER     |"));

  RenderReport report;
  CHECK_EQ(bindString("{{name|frobnicate}}", model, options, &report),
           std::string("pilsner"));
  CHECK_EQ(report.count(ReportKind::UnknownFormatter), static_cast<size_t>(1));

  RenderReport bad_value;
  CHECK_EQ(bindString("{{name|number:2}}", model, options, &bad_value),
           std::string("pilsner"));
  CHECK_EQ(bad_value.count(ReportKind::UnformattableValue), static_cast<size_t>(1));
}

PD_TEST(document_meta_supplies_the_formatting_context) {
  const char* source =
      R"({"meta":{"locale":"cs-CZ","currency":"CZK"},)"
      R"("blocks":[{"text":"{{total|money}}"}]})";
  const BindOutcome outcome = bind(parseDocument(source), parseJson(R"({"total":99})"));
  CHECK_EQ(outcome.document.blocks.at(0).content, std::string("99,00 K\xC4\x8D"));
  // The bound document keeps the meta so the renderer still sees cut and margins.
  CHECK_EQ(outcome.document.meta.locale, std::string("cs-CZ"));

  // Explicit options win over the document's meta.
  BindOptions options;
  options.locale = "en-US";
  options.currency = "USD";
  const BindOutcome overridden =
      bind(parseDocument(source), parseJson(R"({"total":99})"), options);
  CHECK_EQ(overridden.document.blocks.at(0).content, std::string("$99.00"));
}

PD_TEST(binding_a_plain_document_changes_nothing) {
  const char* source =
      R"({"v":1,"blocks":[{"text":"static"},{"qr":"abc","size":6,"ec":"M"}]})";
  CHECK_EQ(bound(source, "{}"), std::string(source));
}

PD_TEST(a_runaway_model_is_capped_and_declared) {
  const Document document =
      parseDocument(R"({"blocks":[{"each":"rows","block":{"text":"{{.}}"}}]})");
  Json rows = Json::array({});
  for (int i = 0; i < 50; ++i) {
    rows.push(Json::number(i));
  }
  Json model;
  model.set("rows", std::move(rows));

  BindOptions options;
  options.max_blocks = 10;
  const BindOutcome outcome = bind(document, model, options);
  CHECK_EQ(outcome.document.blocks.size(), static_cast<size_t>(10));
  CHECK_EQ(outcome.report.count(ReportKind::MalformedTemplate), static_cast<size_t>(1));
}
