#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "printerdriver/dsl/json.hpp"
#include "printerdriver/dsl/report.hpp"

// Value formatters (docs/receipt-dsl.md "Repeating groups ... and value formatters"):
//
//   {{v|number:2}}   {{v|money}}   {{v|money:EUR}}   {{v|date:short}}   {{v|date:HH:mm}}
//   {{v|pad:8:right}}   {{v|upper}}   {{v|lower}}   {{v|trunc:20}}
//
// Formatting happens in the core at bind time so every platform renders identically —
// which is only true if the implementation refuses to touch the host locale. Nothing
// here calls setlocale, strtod, strftime or std::put_money; the tables below are the
// entire source of locale truth and they travel with the library.
//
// Three honest limitations, all declared rather than hidden:
//
// 1. **Time zones are fixed-offset only.** There is no tz database in this library and
//    none is vendored. `meta.tz` accepts "Z"/"UTC" and "+02:00"/"-0500" style offsets
//    and converts exactly; an IANA name such as "Europe/Prague" cannot be resolved to an
//    offset without knowing the date's DST rules, so it is reported through the render
//    report (ReportKind::UnsupportedTimezone) and the timestamp is rendered in the
//    offset it already carries. Guessing +01:00 for half the year is worse than saying so.
// 2. **Grouping separators are ASCII.** CLDR uses U+00A0 for cs-CZ, which does not
//    survive transliteration to a single-byte printer code page — it would print as '?'.
//    A plain space is what actually reaches the paper.
// 3. **Rounding is half-away-from-zero**, the rule receipts and cash registers use.

namespace pd::dsl {

struct LocaleData {
  std::string tag = "en-US";
  std::string group_separator = ",";
  std::string decimal_separator = ".";
  int group_size = 3;
  std::string default_currency = "USD";
  bool currency_symbol_leading = true;
  std::string currency_gap;  // between the symbol and the digits

  std::string date_short = "MM/dd/yyyy";
  std::string date_medium = "MMM d, yyyy";
  std::string date_long = "MMMM d, yyyy";
  std::string time_form = "h:mm a";
  std::string datetime_form = "MM/dd/yyyy h:mm a";

  std::vector<std::string> month_short;  // 12 entries, January first
  std::vector<std::string> month_long;
  std::vector<std::string> day_short;    // 7 entries, Sunday first
  std::vector<std::string> day_long;
  std::string am = "AM";
  std::string pm = "PM";
};

struct CurrencyData {
  std::string code = "USD";
  std::string symbol = "$";
  int decimals = 2;
};

// The shipped tables plus whatever the embedding application adds. Held by value in the
// bind options rather than in a global, so two documents can bind with different tables
// on two threads without a mutex.
struct FormatTables {
  std::vector<LocaleData> locales;
  std::vector<CurrencyData> currencies;

  // cs-CZ and en-US; CZK, EUR, USD and GBP.
  static FormatTables builtin();

  void addLocale(LocaleData locale);      // replaces an existing tag
  void addCurrency(CurrencyData currency);

  // Exact tag, then language prefix ("cs" finds "cs-CZ"), then the first entry.
  const LocaleData& locale(std::string_view tag) const;
  const CurrencyData* currency(std::string_view code) const;
};

struct FormatContext {
  const FormatTables* tables = nullptr;  // nullptr → the builtin tables
  std::string locale;                    // empty → "en-US"
  std::string currency;                  // empty → the locale's default currency
  std::string tz;                        // empty → render in the value's own offset
};

struct FormatOutcome {
  std::string text;
  bool ok = true;
  ReportKind kind = ReportKind::Note;
  std::string detail;
};

// A JSON scalar as it prints with no formatter: strings verbatim, numbers as the model
// wrote them, booleans as true/false, null as empty. Arrays and objects have no receipt
// representation and come back as compact JSON (the caller reports that).
std::string valueToText(const Json& value);

// `spec` is one formatter without the leading '|': "number:2", "date:HH:mm", "upper".
FormatOutcome applyFormatter(const Json& value, std::string_view spec,
                             const FormatContext& context);

// --- primitives, exposed because they are what the goldens test ---------------------

std::string formatNumber(double value, int decimals, const LocaleData& locale);
std::string formatMoney(double value, const LocaleData& locale,
                        const CurrencyData& currency);

struct DateTime {
  int year = 1970;
  int month = 1;  // 1-12
  int day = 1;    // 1-31
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;
  bool has_time = false;
  bool has_offset = false;
  int offset_minutes = 0;

  int weekday() const noexcept;  // 0 = Sunday
};

// Accepts YYYY-MM-DD, optional 'T' or ' ' then HH:MM[:SS[.fff]], optional 'Z' or ±HH:MM.
bool parseIso8601(std::string_view text, DateTime* out);
// "Z", "UTC", "GMT", "+02:00", "-0500", "+02" — anything else is not a fixed offset.
bool parseFixedOffset(std::string_view zone, int* minutes);
DateTime shiftToOffset(const DateTime& value, int target_offset_minutes);

// Pattern tokens: yyyy yy MMMM MMM MM M dd d HH H hh h mm ss SSS a EEEE EEE.
// Anything inside single quotes is literal; '' is a literal quote.
std::string formatDateTime(const DateTime& value, std::string_view pattern,
                           const LocaleData& locale);

}  // namespace pd::dsl
