#include "printerdriver/dsl/format.hpp"

#include <algorithm>
#include <cmath>

#include "printerdriver/dsl/text.hpp"

namespace pd::dsl {
namespace {

std::string lowered(std::string_view value) {
  std::string out(value);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

std::string uppered(std::string_view value) {
  std::string out(value);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 32);
    }
  }
  return out;
}

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

// Saturating: "trunc:99999999999999999999" is a very wide column, not undefined
// behaviour from std::atoi.
size_t parseCount(std::string_view text, size_t cap) {
  size_t value = 0;
  for (const char c : text) {
    value = value * 10 + static_cast<size_t>(c - '0');
    if (value >= cap) {
      return cap;
    }
  }
  return value;
}

// Locale-free decimal parsing. A model value may arrive as a JSON number or as the
// string "1234.50" (a lot of POS backends serialize money as a string to dodge float
// error), and both have to format the same way.
bool parseDecimal(std::string_view text, double* out) {
  size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  const size_t start = i;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  double integral = 0.0;
  bool any_digit = false;
  while (i < text.size() && isDigit(text[i])) {
    integral = integral * 10.0 + static_cast<double>(text[i] - '0');
    ++i;
    any_digit = true;
  }
  double fraction = 0.0;
  double scale = 1.0;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && isDigit(text[i])) {
      scale *= 10.0;
      fraction = fraction * 10.0 + static_cast<double>(text[i] - '0');
      ++i;
      any_digit = true;
    }
  }
  if (!any_digit) {
    return false;
  }
  double value = integral + fraction / scale;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    bool exponent_negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
      exponent_negative = text[i] == '-';
      ++i;
    }
    int exponent = 0;
    bool exponent_digit = false;
    while (i < text.size() && isDigit(text[i])) {
      exponent = exponent * 10 + (text[i] - '0');
      ++i;
      exponent_digit = true;
    }
    if (!exponent_digit) {
      return false;
    }
    value *= std::pow(10.0, exponent_negative ? -exponent : exponent);
  }
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  if (i != text.size() || start == text.size()) {
    return false;
  }
  *out = negative ? -value : value;
  return true;
}

bool numericValue(const Json& value, double* out) {
  if (value.isNumber()) {
    *out = value.asNumber();
    return true;
  }
  if (value.isString()) {
    return parseDecimal(value.asString(), out);
  }
  return false;
}

std::string digitsOf(unsigned long long value) {
  if (value == 0) {
    return "0";
  }
  std::string digits;
  while (value != 0) {
    digits.push_back(static_cast<char>('0' + (value % 10)));
    value /= 10;
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

std::string pad2(int value) {
  std::string digits = digitsOf(static_cast<unsigned long long>(std::abs(value)));
  while (digits.size() < 2) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

std::string pad3(int value) {
  std::string digits = digitsOf(static_cast<unsigned long long>(std::abs(value)));
  while (digits.size() < 3) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

std::string pad4(int value) {
  std::string digits = digitsOf(static_cast<unsigned long long>(std::abs(value)));
  while (digits.size() < 4) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

std::string group(const std::string& digits, const LocaleData& locale) {
  if (locale.group_size <= 0 || locale.group_separator.empty()) {
    return digits;
  }
  std::string out;
  const auto size = static_cast<int>(digits.size());
  for (int i = 0; i < size; ++i) {
    if (i != 0 && (size - i) % locale.group_size == 0) {
      out += locale.group_separator;
    }
    out.push_back(digits[static_cast<size_t>(i)]);
  }
  return out;
}

// Howard Hinnant's civil calendar algorithms: no <ctime>, no time zone database, no
// 32-bit time_t, identical answers on every platform.
long long daysFromCivil(int year, int month, int day) {
  long long y = year;
  y -= month <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned long long>(y - era * 400);
  const auto doy = static_cast<unsigned long long>(
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  const unsigned long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civilFromDays(long long days, int* year, int* month, int* day) {
  days += 719468;
  const long long era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned long long>(days - era * 146097);
  const unsigned long long yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y = static_cast<long long>(yoe) + era * 400;
  const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned long long mp = (5 * doy + 2) / 153;
  const unsigned long long d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned long long m = mp + (mp < 10 ? 3 : -9);
  *year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  *month = static_cast<int>(m);
  *day = static_cast<int>(d);
}

const LocaleData& fallbackLocale() {
  static const LocaleData locale = [] {
    LocaleData data;
    data.month_short = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    data.month_long = {"January", "February", "March",     "April",   "May",      "June",
                       "July",    "August",   "September", "October", "November", "December"};
    data.day_short = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    data.day_long = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                     "Saturday"};
    return data;
  }();
  return locale;
}

std::string monthName(const LocaleData& locale, int month, bool long_form) {
  const std::vector<std::string>& names =
      long_form ? (locale.month_long.size() == 12 ? locale.month_long
                                                  : fallbackLocale().month_long)
                : (locale.month_short.size() == 12 ? locale.month_short
                                                   : fallbackLocale().month_short);
  const int index = std::min(12, std::max(1, month)) - 1;
  return names[static_cast<size_t>(index)];
}

std::string dayName(const LocaleData& locale, int weekday, bool long_form) {
  const std::vector<std::string>& names =
      long_form
          ? (locale.day_long.size() == 7 ? locale.day_long : fallbackLocale().day_long)
          : (locale.day_short.size() == 7 ? locale.day_short : fallbackLocale().day_short);
  const int index = std::min(6, std::max(0, weekday));
  return names[static_cast<size_t>(index)];
}

bool matches(std::string_view pattern, size_t position, std::string_view token) {
  return pattern.compare(position, token.size(), token) == 0;
}

}  // namespace

// --- tables ---------------------------------------------------------------------------

FormatTables FormatTables::builtin() {
  FormatTables tables;

  LocaleData en_us = fallbackLocale();
  en_us.tag = "en-US";
  tables.locales.push_back(en_us);

  LocaleData cs_cz;
  cs_cz.tag = "cs-CZ";
  // CLDR says U+00A0 here; see the header for why this library ships a plain space.
  cs_cz.group_separator = " ";
  cs_cz.decimal_separator = ",";
  cs_cz.default_currency = "CZK";
  cs_cz.currency_symbol_leading = false;
  cs_cz.currency_gap = " ";
  cs_cz.date_short = "dd.MM.yyyy";
  cs_cz.date_medium = "d. MMM yyyy";
  cs_cz.date_long = "d. MMMM yyyy";
  cs_cz.time_form = "HH:mm";
  cs_cz.datetime_form = "dd.MM.yyyy HH:mm";
  cs_cz.month_short = {"led", "úno", "bře", "dub", "kvě", "čvn",
                       "čvc", "srp", "zář", "říj", "lis", "pro"};
  cs_cz.month_long = {"ledna",   "února", "března",  "dubna",    "května",   "června",
                      "července", "srpna", "září",   "října",    "listopadu", "prosince"};
  cs_cz.day_short = {"ne", "po", "út", "st", "čt", "pá", "so"};
  cs_cz.day_long = {"neděle", "pondělí", "úterý", "středa", "čtvrtek", "pátek", "sobota"};
  cs_cz.am = "dop.";
  cs_cz.pm = "odp.";
  tables.locales.push_back(cs_cz);

  tables.currencies.push_back(CurrencyData{"CZK", "Kč", 2});
  tables.currencies.push_back(CurrencyData{"EUR", "€", 2});
  tables.currencies.push_back(CurrencyData{"USD", "$", 2});
  tables.currencies.push_back(CurrencyData{"GBP", "£", 2});
  return tables;
}

void FormatTables::addLocale(LocaleData locale) {
  for (LocaleData& existing : locales) {
    if (lowered(existing.tag) == lowered(locale.tag)) {
      existing = std::move(locale);
      return;
    }
  }
  locales.push_back(std::move(locale));
}

void FormatTables::addCurrency(CurrencyData currency) {
  for (CurrencyData& existing : currencies) {
    if (uppered(existing.code) == uppered(currency.code)) {
      existing = std::move(currency);
      return;
    }
  }
  currencies.push_back(std::move(currency));
}

const LocaleData& FormatTables::locale(std::string_view tag) const {
  if (locales.empty()) {
    return fallbackLocale();
  }
  const std::string wanted = lowered(tag);
  for (const LocaleData& candidate : locales) {
    if (lowered(candidate.tag) == wanted) {
      return candidate;
    }
  }
  const std::string language = wanted.substr(0, wanted.find('-'));
  if (!language.empty()) {
    for (const LocaleData& candidate : locales) {
      if (lowered(candidate.tag).substr(0, lowered(candidate.tag).find('-')) == language) {
        return candidate;
      }
    }
  }
  return locales.front();
}

const CurrencyData* FormatTables::currency(std::string_view code) const {
  const std::string wanted = uppered(code);
  for (const CurrencyData& candidate : currencies) {
    if (uppered(candidate.code) == wanted) {
      return &candidate;
    }
  }
  return nullptr;
}

// --- numbers ----------------------------------------------------------------------------

std::string formatNumber(double value, int decimals, const LocaleData& locale) {
  if (!std::isfinite(value)) {
    return "0";
  }
  decimals = std::min(9, std::max(0, decimals));
  const bool negative = value < 0;
  long double magnitude = std::fabs(static_cast<long double>(value));
  long double scale = 1.0L;
  for (int i = 0; i < decimals; ++i) {
    scale *= 10.0L;
  }
  // Half away from zero: what a cash register does, and what an auditor expects.
  const long double scaled = std::floor(magnitude * scale + 0.5L);
  std::string digits;
  if (scaled < 1.8e19L) {
    digits = digitsOf(static_cast<unsigned long long>(scaled));
  } else {
    // Beyond 64-bit precision the value is not a receipt amount any more; print the
    // integral part and pad the decimals with zeros rather than lying about precision.
    digits = digitsOf(static_cast<unsigned long long>(magnitude));
    digits.append(static_cast<size_t>(decimals), '0');
  }
  while (digits.size() <= static_cast<size_t>(decimals)) {
    digits.insert(digits.begin(), '0');
  }
  const std::string integral = digits.substr(0, digits.size() - static_cast<size_t>(decimals));
  const std::string fraction = digits.substr(digits.size() - static_cast<size_t>(decimals));

  std::string out;
  if (negative && (integral != "0" || fraction.find_first_not_of('0') != std::string::npos)) {
    out += "-";
  }
  out += group(integral, locale);
  if (decimals > 0) {
    out += locale.decimal_separator;
    out += fraction;
  }
  return out;
}

std::string formatMoney(double value, const LocaleData& locale,
                        const CurrencyData& currency) {
  const std::string amount = formatNumber(value, currency.decimals, locale);
  if (locale.currency_symbol_leading) {
    return currency.symbol + locale.currency_gap + amount;
  }
  return amount + (locale.currency_gap.empty() ? " " : locale.currency_gap) +
         currency.symbol;
}

// --- dates ------------------------------------------------------------------------------

int DateTime::weekday() const noexcept {
  const long long days = daysFromCivil(year, month, day);
  // 1970-01-01 was a Thursday (4).
  const long long weekday = (days + 4) % 7;
  return static_cast<int>(weekday < 0 ? weekday + 7 : weekday);
}

bool parseFixedOffset(std::string_view zone, int* minutes) {
  if (zone.empty()) {
    return false;
  }
  const std::string upper = uppered(zone);
  if (upper == "Z" || upper == "UTC" || upper == "GMT" || upper == "UTC+0" ||
      upper == "+00:00") {
    *minutes = 0;
    return true;
  }
  size_t i = 0;
  if (upper.compare(0, 3, "UTC") == 0) {
    i = 3;
  } else if (upper.compare(0, 3, "GMT") == 0) {
    i = 3;
  }
  if (i >= upper.size() || (upper[i] != '+' && upper[i] != '-')) {
    return false;
  }
  const bool negative = upper[i] == '-';
  ++i;
  int hours = 0;
  int mins = 0;
  size_t digits = 0;
  while (i < upper.size() && isDigit(upper[i]) && digits < 2) {
    hours = hours * 10 + (upper[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0) {
    return false;
  }
  if (i < upper.size() && upper[i] == ':') {
    ++i;
  }
  digits = 0;
  while (i < upper.size() && isDigit(upper[i]) && digits < 2) {
    mins = mins * 10 + (upper[i] - '0');
    ++i;
    ++digits;
  }
  if (i != upper.size() || hours > 23 || mins > 59) {
    return false;
  }
  const int total = hours * 60 + mins;
  *minutes = negative ? -total : total;
  return true;
}

bool parseIso8601(std::string_view input, DateTime* out) {
  DateTime value;
  size_t i = 0;
  const std::string text(input);
  auto readInt = [&](size_t count, int* target) {
    if (i + count > text.size()) {
      return false;
    }
    int result = 0;
    for (size_t k = 0; k < count; ++k) {
      if (!isDigit(text[i + k])) {
        return false;
      }
      result = result * 10 + (text[i + k] - '0');
    }
    i += count;
    *target = result;
    return true;
  };

  if (!readInt(4, &value.year)) {
    return false;
  }
  if (i >= text.size() || text[i] != '-') {
    return false;
  }
  ++i;
  if (!readInt(2, &value.month)) {
    return false;
  }
  if (i >= text.size() || text[i] != '-') {
    return false;
  }
  ++i;
  if (!readInt(2, &value.day)) {
    return false;
  }
  if (value.month < 1 || value.month > 12 || value.day < 1 || value.day > 31) {
    return false;
  }

  if (i < text.size() && (text[i] == 'T' || text[i] == 't' || text[i] == ' ')) {
    ++i;
    if (!readInt(2, &value.hour)) {
      return false;
    }
    if (i >= text.size() || text[i] != ':') {
      return false;
    }
    ++i;
    if (!readInt(2, &value.minute)) {
      return false;
    }
    value.has_time = true;
    if (i < text.size() && text[i] == ':') {
      ++i;
      if (!readInt(2, &value.second)) {
        return false;
      }
    }
    if (i < text.size() && text[i] == '.') {
      ++i;
      int digits = 0;
      int fraction = 0;
      while (i < text.size() && isDigit(text[i])) {
        if (digits < 3) {
          fraction = fraction * 10 + (text[i] - '0');
          ++digits;
        }
        ++i;
      }
      while (digits < 3) {
        fraction *= 10;
        ++digits;
      }
      value.millisecond = fraction;
    }
    if (value.hour > 23 || value.minute > 59 || value.second > 60) {
      return false;
    }
    if (i < text.size()) {
      int offset = 0;
      if (!parseFixedOffset(text.substr(i), &offset)) {
        return false;
      }
      value.has_offset = true;
      value.offset_minutes = offset;
      i = text.size();
    }
  }
  if (i != text.size()) {
    return false;
  }
  *out = value;
  return true;
}

DateTime shiftToOffset(const DateTime& value, int target_offset_minutes) {
  if (!value.has_time) {
    return value;
  }
  DateTime out = value;
  const int delta = target_offset_minutes - (value.has_offset ? value.offset_minutes : 0);
  long long minutes = static_cast<long long>(value.hour) * 60 + value.minute + delta;
  long long day_shift = 0;
  while (minutes < 0) {
    minutes += 24 * 60;
    --day_shift;
  }
  while (minutes >= 24 * 60) {
    minutes -= 24 * 60;
    ++day_shift;
  }
  out.hour = static_cast<int>(minutes / 60);
  out.minute = static_cast<int>(minutes % 60);
  if (day_shift != 0) {
    const long long days = daysFromCivil(value.year, value.month, value.day) + day_shift;
    civilFromDays(days, &out.year, &out.month, &out.day);
  }
  out.has_offset = true;
  out.offset_minutes = target_offset_minutes;
  return out;
}

std::string formatDateTime(const DateTime& value, std::string_view pattern,
                           const LocaleData& locale) {
  std::string out;
  size_t i = 0;
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (c == '\'') {
      ++i;
      if (i < pattern.size() && pattern[i] == '\'') {
        out.push_back('\'');
        ++i;
        continue;
      }
      while (i < pattern.size() && pattern[i] != '\'') {
        out.push_back(pattern[i]);
        ++i;
      }
      if (i < pattern.size()) {
        ++i;
      }
      continue;
    }
    if (matches(pattern, i, "yyyy")) {
      out += pad4(value.year);
      i += 4;
    } else if (matches(pattern, i, "yy")) {
      out += pad2(value.year % 100);
      i += 2;
    } else if (matches(pattern, i, "MMMM")) {
      out += monthName(locale, value.month, true);
      i += 4;
    } else if (matches(pattern, i, "MMM")) {
      out += monthName(locale, value.month, false);
      i += 3;
    } else if (matches(pattern, i, "MM")) {
      out += pad2(value.month);
      i += 2;
    } else if (matches(pattern, i, "M")) {
      out += digitsOf(static_cast<unsigned long long>(value.month));
      i += 1;
    } else if (matches(pattern, i, "dd")) {
      out += pad2(value.day);
      i += 2;
    } else if (matches(pattern, i, "d")) {
      out += digitsOf(static_cast<unsigned long long>(value.day));
      i += 1;
    } else if (matches(pattern, i, "EEEE")) {
      out += dayName(locale, value.weekday(), true);
      i += 4;
    } else if (matches(pattern, i, "EEE")) {
      out += dayName(locale, value.weekday(), false);
      i += 3;
    } else if (matches(pattern, i, "HH")) {
      out += pad2(value.hour);
      i += 2;
    } else if (matches(pattern, i, "H")) {
      out += digitsOf(static_cast<unsigned long long>(value.hour));
      i += 1;
    } else if (matches(pattern, i, "hh")) {
      const int hour = value.hour % 12 == 0 ? 12 : value.hour % 12;
      out += pad2(hour);
      i += 2;
    } else if (matches(pattern, i, "h")) {
      const int hour = value.hour % 12 == 0 ? 12 : value.hour % 12;
      out += digitsOf(static_cast<unsigned long long>(hour));
      i += 1;
    } else if (matches(pattern, i, "mm")) {
      out += pad2(value.minute);
      i += 2;
    } else if (matches(pattern, i, "ss")) {
      out += pad2(value.second);
      i += 2;
    } else if (matches(pattern, i, "SSS")) {
      out += pad3(value.millisecond);
      i += 3;
    } else if (matches(pattern, i, "a")) {
      out += value.hour < 12 ? locale.am : locale.pm;
      i += 1;
    } else {
      out.push_back(c);
      ++i;
    }
  }
  return out;
}

// --- the formatter entry point -------------------------------------------------------

std::string valueToText(const Json& value) {
  switch (value.type()) {
    case Json::Type::Null:
      return std::string();
    case Json::Type::Bool:
      return value.asBool() ? "true" : "false";
    case Json::Type::Number:
      return value.literal().empty() ? toJson(value) : value.literal();
    case Json::Type::String:
      return value.asString();
    case Json::Type::Array:
    case Json::Type::Object:
      return toJson(value);
  }
  return std::string();
}

FormatOutcome applyFormatter(const Json& value, std::string_view spec,
                             const FormatContext& context) {
  static const FormatTables kBuiltin = FormatTables::builtin();
  const FormatTables& tables = context.tables != nullptr ? *context.tables : kBuiltin;
  const LocaleData& locale =
      tables.locale(context.locale.empty() ? std::string("en-US") : context.locale);

  FormatOutcome outcome;
  outcome.text = valueToText(value);

  std::string name(spec);
  std::string argument;
  const size_t colon = name.find(':');
  if (colon != std::string::npos) {
    argument = name.substr(colon + 1);
    name = name.substr(0, colon);
  }
  // Trim: "{{ v | number:2 }}" is the same as "{{v|number:2}}".
  const auto trim = [](std::string& target) {
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) {
      target.erase(target.begin());
    }
    while (!target.empty() && (target.back() == ' ' || target.back() == '\t')) {
      target.pop_back();
    }
  };
  trim(name);
  const std::string key = lowered(name);

  if (key == "upper") {
    outcome.text = text::upper(outcome.text);
    return outcome;
  }
  if (key == "lower") {
    outcome.text = text::lower(outcome.text);
    return outcome;
  }
  if (key == "trunc") {
    trim(argument);
    if (argument.empty() || !std::all_of(argument.begin(), argument.end(), isDigit)) {
      outcome.ok = false;
      outcome.kind = ReportKind::UnknownFormatter;
      outcome.detail = "trunc needs a character count, e.g. trunc:20";
      return outcome;
    }
    outcome.text = text::clip(outcome.text, parseCount(argument, 4096));
    return outcome;
  }
  if (key == "pad") {
    std::string width_text = argument;
    std::string side = "right";
    const size_t second = argument.find(':');
    if (second != std::string::npos) {
      width_text = argument.substr(0, second);
      side = lowered(argument.substr(second + 1));
    }
    trim(width_text);
    trim(side);
    if (width_text.empty() || !std::all_of(width_text.begin(), width_text.end(), isDigit)) {
      outcome.ok = false;
      outcome.kind = ReportKind::UnknownFormatter;
      outcome.detail = "pad needs a width, e.g. pad:8:right";
      return outcome;
    }
    const size_t width = parseCount(width_text, 4096);
    // The side names where the padding goes: pad:8:right leaves the text left-aligned.
    Align align = Align::Left;
    if (side == "left") {
      align = Align::Right;
    } else if (side == "both" || side == "center" || side == "centre") {
      align = Align::Center;
    } else if (side != "right" && !side.empty()) {
      outcome.ok = false;
      outcome.kind = ReportKind::UnknownFormatter;
      outcome.detail = "pad side must be left, right or both";
      outcome.text = text::pad(outcome.text, width, Align::Left);
      return outcome;
    }
    outcome.text = text::pad(outcome.text, width, align);
    return outcome;
  }
  if (key == "number" || key == "money") {
    double numeric = 0.0;
    if (!numericValue(value, &numeric)) {
      outcome.ok = false;
      outcome.kind = ReportKind::UnformattableValue;
      outcome.detail = "not a number";
      return outcome;
    }
    if (key == "number") {
      trim(argument);
      if (argument.empty()) {
        // Bare `number` groups the digits and keeps whatever precision the value has,
        // up to six places, with trailing zeros trimmed.
        std::string text = formatNumber(numeric, 6, locale);
        const size_t point = text.rfind(locale.decimal_separator);
        if (point != std::string::npos) {
          while (!text.empty() && text.back() == '0') {
            text.pop_back();
          }
          if (text.size() == point + locale.decimal_separator.size()) {
            text.erase(point);
          }
        }
        outcome.text = text;
        return outcome;
      }
      if (!std::all_of(argument.begin(), argument.end(), isDigit)) {
        outcome.ok = false;
        outcome.kind = ReportKind::UnknownFormatter;
        outcome.detail = "number needs a decimal count, e.g. number:2";
        return outcome;
      }
      outcome.text = formatNumber(numeric, static_cast<int>(parseCount(argument, 9)), locale);
      return outcome;
    }
    trim(argument);
    std::string code = argument.empty() ? context.currency : argument;
    if (code.empty()) {
      code = locale.default_currency;
    }
    const CurrencyData* currency = tables.currency(code);
    if (currency == nullptr) {
      // An unknown currency still prints: the code stands in for the symbol.
      CurrencyData improvised;
      improvised.code = uppered(code);
      improvised.symbol = uppered(code);
      outcome.text = formatMoney(numeric, locale, improvised);
      outcome.ok = false;
      outcome.kind = ReportKind::UnformattableValue;
      outcome.detail = "no symbol for currency " + improvised.code;
      return outcome;
    }
    outcome.text = formatMoney(numeric, locale, *currency);
    return outcome;
  }
  if (key == "date") {
    if (!value.isString()) {
      outcome.ok = false;
      outcome.kind = ReportKind::UnformattableValue;
      outcome.detail = "date input must be an ISO-8601 string";
      return outcome;
    }
    DateTime parsed;
    if (!parseIso8601(value.asString(), &parsed)) {
      outcome.ok = false;
      outcome.kind = ReportKind::UnformattableValue;
      outcome.detail = "not ISO-8601";
      return outcome;
    }
    if (!context.tz.empty()) {
      int offset = 0;
      if (parseFixedOffset(context.tz, &offset)) {
        parsed = shiftToOffset(parsed, offset);
      } else {
        // No tz database: say so and render in the offset the value already carries.
        outcome.ok = false;
        outcome.kind = ReportKind::UnsupportedTimezone;
        outcome.detail = "zone \"" + context.tz +
                         "\" is not a fixed offset; rendered without conversion";
      }
    }
    // `argument` is everything after "date:", verbatim, so "date:HH:mm" keeps its colon.
    std::string form = argument;
    trim(form);
    std::string pattern = form;
    const std::string named = lowered(form);
    if (named.empty() || named == "short") {
      pattern = locale.date_short;
    } else if (named == "medium") {
      pattern = locale.date_medium;
    } else if (named == "long") {
      pattern = locale.date_long;
    } else if (named == "time") {
      pattern = locale.time_form;
    } else if (named == "datetime") {
      pattern = locale.datetime_form;
    }
    outcome.text = formatDateTime(parsed, pattern, locale);
    return outcome;
  }

  outcome.ok = false;
  outcome.kind = ReportKind::UnknownFormatter;
  outcome.detail = "no formatter named \"" + name + "\"";
  return outcome;
}

}  // namespace pd::dsl
