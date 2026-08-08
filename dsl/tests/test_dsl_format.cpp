#include <string>

#include "printerdriver/dsl/format.hpp"
#include "test_harness.hpp"

using namespace pd::dsl;

namespace {

const FormatTables& tables() {
  static const FormatTables built = FormatTables::builtin();
  return built;
}

FormatContext czech() {
  FormatContext context;
  context.tables = &tables();
  context.locale = "cs-CZ";
  context.currency = "CZK";
  return context;
}

FormatContext american() {
  FormatContext context;
  context.tables = &tables();
  context.locale = "en-US";
  context.currency = "USD";
  return context;
}

std::string format(const Json& value, const std::string& spec,
                   const FormatContext& context) {
  return applyFormatter(value, spec, context).text;
}

}  // namespace

PD_TEST(number_grouping_follows_the_locale) {
  // docs/receipt-dsl.md: {{v|number:2}}  1234.5 → "1 234,50" (locale grouping/decimal).
  CHECK_EQ(format(Json::number(1234.5), "number:2", czech()), std::string("1 234,50"));
  CHECK_EQ(format(Json::number(1234.5), "number:2", american()), std::string("1,234.50"));

  CHECK_EQ(formatNumber(0, 2, tables().locale("cs-CZ")), std::string("0,00"));
  CHECK_EQ(formatNumber(-1234.567, 2, tables().locale("cs-CZ")), std::string("-1 234,57"));
  CHECK_EQ(formatNumber(1234567.891, 3, tables().locale("en-US")),
           std::string("1,234,567.891"));
  CHECK_EQ(formatNumber(999.996, 2, tables().locale("en-US")), std::string("1,000.00"));
  CHECK_EQ(formatNumber(12, 0, tables().locale("en-US")), std::string("12"));
  // Half away from zero, the cash-register rule.
  CHECK_EQ(formatNumber(2.5, 0, tables().locale("en-US")), std::string("3"));
  CHECK_EQ(formatNumber(-2.5, 0, tables().locale("en-US")), std::string("-3"));
  CHECK_EQ(formatNumber(0.125, 2, tables().locale("en-US")), std::string("0.13"));

  // Bare `number` keeps the value's own precision and drops trailing zeros.
  CHECK_EQ(format(Json::number(1234.5), "number", american()), std::string("1,234.5"));
  CHECK_EQ(format(Json::number(1234), "number", american()), std::string("1,234"));
}

PD_TEST(money_uses_the_currency_table_and_the_locale_pattern) {
  // docs/receipt-dsl.md: {{v|money}} → "1 234,50 Kč"  ({{v|money:EUR}} to override).
  CHECK_EQ(format(Json::number(1234.5), "money", czech()), std::string("1 234,50 K\xC4\x8D"));
  CHECK_EQ(format(Json::number(1234.5), "money:EUR", czech()),
           std::string("1 234,50 \xE2\x82\xAC"));
  CHECK_EQ(format(Json::number(1234.5), "money", american()), std::string("$1,234.50"));
  CHECK_EQ(format(Json::number(9), "money:GBP", american()),
           std::string("\xC2\xA3") + "9.00");

  // An empty currency falls back to the locale's own.
  FormatContext bare;
  bare.tables = &tables();
  bare.locale = "cs-CZ";
  CHECK_EQ(format(Json::number(5), "money", bare), std::string("5,00 K\xC4\x8D"));

  // Money strings are common in POS models and must format identically to numbers.
  CHECK_EQ(format(Json::string("1234.50"), "money", czech()),
           std::string("1 234,50 K\xC4\x8D"));
}

PD_TEST(unknown_currency_still_prints_and_says_so) {
  const FormatOutcome outcome = applyFormatter(Json::number(12), "money:XYZ", american());
  CHECK(!outcome.ok);
  CHECK(outcome.kind == ReportKind::UnformattableValue);
  CHECK_EQ(outcome.text, std::string("XYZ12.00"));
}

PD_TEST(extra_locales_and_currencies_can_be_registered) {
  FormatTables custom = FormatTables::builtin();
  LocaleData de;
  de.tag = "de-DE";
  de.group_separator = ".";
  de.decimal_separator = ",";
  de.default_currency = "EUR";
  de.currency_symbol_leading = false;
  de.currency_gap = " ";
  custom.addLocale(de);
  custom.addCurrency(CurrencyData{"PLN", "zł", 2});

  FormatContext context;
  context.tables = &custom;
  context.locale = "de-DE";
  CHECK_EQ(format(Json::number(1234.5), "number:2", context), std::string("1.234,50"));
  CHECK_EQ(format(Json::number(1234.5), "money", context),
           std::string("1.234,50 \xE2\x82\xAC"));
  CHECK_EQ(format(Json::number(7), "money:PLN", context), std::string("7,00 z\xC5\x82"));

  // Language-prefix fallback: "cs" finds "cs-CZ".
  context.locale = "cs";
  CHECK_EQ(format(Json::number(1234.5), "number:2", context), std::string("1 234,50"));
  // An unknown tag falls back to the first table entry rather than failing.
  context.locale = "xx-XX";
  CHECK_EQ(format(Json::number(1234.5), "number:2", context), std::string("1,234.50"));
}

PD_TEST(iso8601_parsing_covers_the_shapes_a_model_sends) {
  DateTime value;
  CHECK(parseIso8601("2026-08-08", &value));
  CHECK_EQ(value.year, 2026);
  CHECK_EQ(value.month, 8);
  CHECK_EQ(value.day, 8);
  CHECK(!value.has_time);

  CHECK(parseIso8601("2026-08-08T14:05:09", &value));
  CHECK(value.has_time);
  CHECK_EQ(value.hour, 14);
  CHECK_EQ(value.second, 9);
  CHECK(!value.has_offset);

  CHECK(parseIso8601("2026-08-08T14:05:09.250Z", &value));
  CHECK_EQ(value.millisecond, 250);
  CHECK(value.has_offset);
  CHECK_EQ(value.offset_minutes, 0);

  CHECK(parseIso8601("2026-08-08 14:05+02:00", &value));
  CHECK_EQ(value.offset_minutes, 120);
  CHECK_EQ(value.weekday(), 6);  // 2026-08-08 is a Saturday

  CHECK(!parseIso8601("08/08/2026", &value));
  CHECK(!parseIso8601("2026-08-08T25:00", &value));
  CHECK(!parseIso8601("2026-13-01", &value));
  CHECK(!parseIso8601("2026-08-08T14:05:09 extra", &value));
  CHECK(!parseIso8601("", &value));
}

PD_TEST(named_date_forms_and_patterns) {
  // docs/receipt-dsl.md: {{v|date:short}} ISO input → "08/08/2026".
  const Json stamp = Json::string("2026-08-08T14:05:09");
  CHECK_EQ(format(stamp, "date:short", american()), std::string("08/08/2026"));
  CHECK_EQ(format(stamp, "date:medium", american()), std::string("Aug 8, 2026"));
  CHECK_EQ(format(stamp, "date:long", american()), std::string("August 8, 2026"));
  CHECK_EQ(format(stamp, "date:time", american()), std::string("2:05 PM"));
  CHECK_EQ(format(stamp, "date:datetime", american()), std::string("08/08/2026 2:05 PM"));

  CHECK_EQ(format(stamp, "date:short", czech()), std::string("08.08.2026"));
  CHECK_EQ(format(stamp, "date:long", czech()), std::string("8. srpna 2026"));
  CHECK_EQ(format(stamp, "date:time", czech()), std::string("14:05"));

  // Pattern form: everything after "date:" is the pattern, colons included.
  CHECK_EQ(format(stamp, "date:HH:mm", american()), std::string("14:05"));
  CHECK_EQ(format(stamp, "date:HH:mm:ss", american()), std::string("14:05:09"));
  CHECK_EQ(format(stamp, "date:yyyy-MM-dd", american()), std::string("2026-08-08"));
  CHECK_EQ(format(stamp, "date:EEE d MMM yyyy", american()),
           std::string("Sat 8 Aug 2026"));
  CHECK_EQ(format(stamp, "date:'at' HH'h'", american()), std::string("at 14h"));
  // Bare `date` is the locale's short form.
  CHECK_EQ(format(stamp, "date", american()), std::string("08/08/2026"));
}

PD_TEST(timezones_are_fixed_offset_only_and_say_so) {
  const Json stamp = Json::string("2026-08-08T23:30:00Z");

  FormatContext fixed = american();
  fixed.tz = "+02:00";
  FormatOutcome shifted = applyFormatter(stamp, "date:datetime", fixed);
  CHECK(shifted.ok);
  CHECK_EQ(shifted.text, std::string("08/09/2026 1:30 AM"));

  fixed.tz = "-05:30";
  shifted = applyFormatter(stamp, "date:yyyy-MM-dd HH:mm", fixed);
  CHECK_EQ(shifted.text, std::string("2026-08-08 18:00"));

  // An IANA zone name has no offset without a tz database: declared, not guessed.
  FormatContext prague = czech();
  prague.tz = "Europe/Prague";
  const FormatOutcome unresolved = applyFormatter(stamp, "date:HH:mm", prague);
  CHECK(!unresolved.ok);
  CHECK(unresolved.kind == ReportKind::UnsupportedTimezone);
  CHECK_EQ(unresolved.text, std::string("23:30"));  // rendered in its own offset

  CHECK(shiftToOffset(DateTime{}, 60).has_offset == false);  // date-only values do not move
}

PD_TEST(pad_trunc_upper_and_lower) {
  // docs/receipt-dsl.md: {{v|pad:8:right}} fixed-width padding.
  // The side names where the padding goes, so pad:8:right stays left-aligned.
  CHECK_EQ(format(Json::string("abc"), "pad:8:right", american()), std::string("abc     "));
  CHECK_EQ(format(Json::string("abc"), "pad:8:left", american()), std::string("     abc"));
  CHECK_EQ(format(Json::string("abc"), "pad:8:both", american()), std::string("  abc   "));
  CHECK_EQ(format(Json::string("abc"), "pad:8", american()), std::string("abc     "));
  CHECK_EQ(format(Json::string("abcdefghij"), "pad:4:right", american()),
           std::string("abcd"));

  CHECK_EQ(format(Json::string("Grilovaná klobása"), "trunc:9", american()),
           std::string("Grilovan\xC3\xA1"));
  CHECK_EQ(format(Json::string("short"), "trunc:20", american()), std::string("short"));
  // Absurd widths saturate rather than overflow.
  CHECK_EQ(format(Json::string("short"), "trunc:99999999999999999999", american()),
           std::string("short"));
  CHECK_EQ(format(Json::string("x"), "pad:99999999999999999999:right", american()).size(),
           static_cast<size_t>(4096));

  CHECK_EQ(format(Json::string("čaj s mlékem"), "upper", american()),
           std::string("\xC4\x8C" "AJ S ML" "\xC3\x89" "KEM"));
  CHECK_EQ(format(Json::string("ČAJ"), "lower", american()), std::string("\xC4\x8D" "aj"));
  CHECK_EQ(format(Json::number(42), "upper", american()), std::string("42"));
}

PD_TEST(unknown_and_unformattable_inputs_report_and_keep_the_raw_value) {
  // docs/receipt-dsl.md: unknown formatter or unformattable value → declared warning +
  // raw value printed, never a crash.
  FormatOutcome outcome = applyFormatter(Json::string("x"), "frobnicate", american());
  CHECK(!outcome.ok);
  CHECK(outcome.kind == ReportKind::UnknownFormatter);
  CHECK_EQ(outcome.text, std::string("x"));

  outcome = applyFormatter(Json::string("not a number"), "number:2", american());
  CHECK(!outcome.ok);
  CHECK(outcome.kind == ReportKind::UnformattableValue);
  CHECK_EQ(outcome.text, std::string("not a number"));

  outcome = applyFormatter(Json::string("yesterday"), "date:short", american());
  CHECK(!outcome.ok);
  CHECK_EQ(outcome.text, std::string("yesterday"));

  outcome = applyFormatter(Json::number(5), "date:short", american());
  CHECK(!outcome.ok);
  CHECK_EQ(outcome.text, std::string("5"));

  outcome = applyFormatter(Json::string("x"), "trunc:abc", american());
  CHECK(!outcome.ok);
  CHECK_EQ(outcome.text, std::string("x"));

  outcome = applyFormatter(Json::string("x"), "pad", american());
  CHECK(!outcome.ok);
}

PD_TEST(value_to_text_prints_what_the_model_wrote) {
  CHECK_EQ(valueToText(parseJson("9.00")), std::string("9.00"));
  CHECK_EQ(valueToText(parseJson("true")), std::string("true"));
  CHECK_EQ(valueToText(parseJson("null")), std::string());
  CHECK_EQ(valueToText(parseJson("\"text\"")), std::string("text"));
  CHECK_EQ(valueToText(parseJson("[1,2]")), std::string("[1,2]"));
}
