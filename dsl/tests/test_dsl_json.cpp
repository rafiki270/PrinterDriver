#include <string>
#include <vector>

#include "printerdriver/dsl/json.hpp"
#include "test_harness.hpp"

using namespace pd::dsl;

PD_TEST(parses_the_canonical_receipt_document) {
  // docs/receipt-dsl.md "JSON canonical form (what travels and gets stored)".
  const std::string source = R"JSON(
{ "v": 1,
  "styles": { "h1": { "bold": true, "widthScale": 2, "heightScale": 2, "align": "center" } },
  "blocks": [
    { "text": "MY RESTAURANT", "style": "h1" },
    { "columns": [ { "content": "2× Pilsner", "width": "flex" },
                   { "content": "9.00", "width": { "chars": 8 }, "align": "right" } ] },
    { "qr": "7F3A-92C1", "size": 6, "ec": "M", "align": "center" } ] }
)JSON";

  const Json document = parseJson(source);
  CHECK(document.isObject());
  CHECK_EQ(document.find("v")->asInt(), 1);
  CHECK(document.find("styles")->find("h1")->find("bold")->asBool());
  CHECK_EQ(document.find("blocks")->size(), static_cast<size_t>(3));

  const Json* columns = document.find("blocks")->at(1)->find("columns");
  CHECK_EQ(columns->size(), static_cast<size_t>(2));
  // UTF-8 passes through untouched: the multiplication sign is two bytes, not "?".
  CHECK_EQ(columns->at(0)->find("content")->asString(), std::string("2\xC3\x97 Pilsner"));
  CHECK_EQ(columns->at(1)->find("width")->find("chars")->asInt(), 8);
}

PD_TEST(round_trips_through_serialization) {
  const std::string source =
      R"JSON({"v":1,"styles":{"h1":{"bold":true,"widthScale":2}},)JSON"
      R"JSON("blocks":[{"text":"MY RESTAURANT","style":"h1"},{"feed":2}]})JSON";
  const Json parsed = parseJson(source);
  // Object member order is preserved, so the compact form is byte-identical.
  CHECK_EQ(toJson(parsed), source);
  CHECK(parseJson(toJson(parsed)) == parsed);

  const std::string pretty = toJson(parsed, true);
  CHECK(pretty.find("\n  \"v\": 1") != std::string::npos);
  CHECK(parseJson(pretty) == parsed);
}

PD_TEST(keeps_number_literals_verbatim) {
  // "9.00" must not come back as 9: a receipt shows the digits the model wrote.
  const Json parsed = parseJson(R"({"a":9.00,"b":-0.5,"c":1e3,"d":0,"e":1234.5})");
  CHECK_EQ(toJson(parsed), std::string(R"({"a":9.00,"b":-0.5,"c":1e3,"d":0,"e":1234.5})"));
  CHECK_EQ(parsed.find("a")->asNumber(), 9.0);
  CHECK_EQ(parsed.find("c")->asNumber(), 1000.0);
  CHECK_EQ(parsed.find("b")->asNumber(), -0.5);
  // asInt rounds half away from zero, the same rule the money formatter uses.
  CHECK_EQ(parseJson("2.6").asInt(), 3);
  CHECK_EQ(parseJson("-2.6").asInt(), -3);

  // A number built in code has no literal and formats shortest-round-trip.
  CHECK_EQ(toJson(Json::number(1234.5)), std::string("1234.5"));
  CHECK_EQ(toJson(Json::number(42)), std::string("42"));
  CHECK_EQ(toJson(Json::number(-0.25)), std::string("-0.25"));
}

PD_TEST(decodes_unicode_escapes_including_surrogate_pairs) {
  const Json parsed = parseJson(R"({"a":"K\u010d","b":"\uD83D\uDE00","c":"\/\\\"\n"})");
  CHECK_EQ(parsed.find("a")->asString(), std::string("K\xC4\x8D"));       // "Kč"
  CHECK_EQ(parsed.find("b")->asString(), std::string("\xF0\x9F\x98\x80"));  // U+1F600
  CHECK_EQ(parsed.find("c")->asString(), std::string("/\\\"\n"));

  // Serialization escapes only what must be escaped: '/' stays a slash, UTF-8 stays UTF-8.
  CHECK_EQ(toJson(Json::string("K\xC4\x8D/\n\t\x01")),
           std::string("\"K\xC4\x8D/\\n\\t\\u0001\""));
}

PD_TEST(rejects_trailing_garbage) {
  CHECK_THROWS(parseJson("{} {}"), JsonError);
  CHECK_THROWS(parseJson("[1,2] junk"), JsonError);
  CHECK_THROWS(parseJson("1 2"), JsonError);
  CHECK_THROWS(parseJson("\"a\"\"b\""), JsonError);
  // Whitespace after the value is fine.
  CHECK(parseJson("  {\"a\":1}  \n").isObject());
}

PD_TEST(rejects_malformed_input) {
  // The fuzz-ish set: every one of these must produce an error, never a crash and never
  // a half-parsed document.
  const std::vector<std::string> garbage = {
      "",          " ",        "\n\t",     "{",        "}",        "[",
      "]",         "[,]",      "{,}",      "[1,]",     "{\"a\":1,}", "{\"a\"}",
      "{\"a\":}",  "{a:1}",    "{'a':1}",  "[1 2]",    "tru",      "nulll",
      "TRUE",      "+1",       "01",       "1.",       ".5",       "1e",
      "1e+",       "-",        "--1",      "0x10",     "NaN",      "Infinity",
      "\"unterminated", "\"\\q\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\ud800\"",
      "\"\\udc00\"", "\"\\ud800\\u0041\"", "{\"a\":\"\x01\"}", "[[[[",
      "{\"a\":{\"b\":}}", "'x'", "1,2", "[1;2]", "{\"a\" 1}", "\xFF\xFE",
  };
  for (const std::string& input : garbage) {
    Json parsed;
    std::string error;
    const bool ok = tryParseJson(input, &parsed, &error);
    if (ok) {
      ::pdtest::recordFailure(__FILE__, __LINE__,
                              "expected a parse failure for: " + input);
    } else if (error.empty()) {
      ::pdtest::recordFailure(__FILE__, __LINE__, "empty error message for: " + input);
    }
  }
}

PD_TEST(rejects_runaway_nesting_instead_of_overflowing_the_stack) {
  std::string deep(kMaxJsonDepth + 8, '[');
  deep += std::string(kMaxJsonDepth + 8, ']');
  CHECK_THROWS(parseJson(deep), JsonError);

  // Just inside the limit still parses.
  std::string shallow(kMaxJsonDepth - 1, '[');
  shallow += std::string(kMaxJsonDepth - 1, ']');
  CHECK(parseJson(shallow).isArray());
}

PD_TEST(duplicate_keys_keep_the_last_value) {
  const Json parsed = parseJson(R"({"a":1,"b":2,"a":3})");
  CHECK_EQ(parsed.size(), static_cast<size_t>(2));
  CHECK_EQ(parsed.find("a")->asInt(), 3);
}

PD_TEST(truthiness_matches_the_dsl_rules) {
  // docs/receipt-dsl.md: `if`/`unless` on truthiness.
  CHECK(!parseJson("null").truthy());
  CHECK(!parseJson("false").truthy());
  CHECK(!parseJson("0").truthy());
  CHECK(!parseJson("\"\"").truthy());
  CHECK(!parseJson("[]").truthy());
  CHECK(!parseJson("{}").truthy());
  CHECK(parseJson("true").truthy());
  CHECK(parseJson("0.5").truthy());
  CHECK(parseJson("\" \"").truthy());
  CHECK(parseJson("[0]").truthy());
  CHECK(parseJson("{\"a\":null}").truthy());
}

PD_TEST(builders_and_accessors_are_total) {
  Json object;
  object.set("a", Json::number(1));
  object.set("b", Json::string("x"));
  object.set("a", Json::boolean(true));
  CHECK_EQ(toJson(object), std::string(R"({"a":true,"b":"x"})"));
  object.remove("a");
  CHECK_EQ(toJson(object), std::string(R"({"b":"x"})"));

  Json array;
  array.push(Json::number(1));
  array.push(Json::null());
  CHECK_EQ(toJson(array), std::string("[1,null]"));

  // Accessors never throw on the wrong type: a template that asks a string for its
  // members gets an empty answer, not an exception mid-receipt.
  const Json text = Json::string("hello");
  CHECK(text.find("a") == nullptr);
  CHECK(text.at(0) == nullptr);
  CHECK_EQ(text.asNumber(-1.0), -1.0);
  CHECK(text.asArray().empty());
  CHECK(text.asObject().empty());
  CHECK_EQ(text.size(), static_cast<size_t>(0));
}
