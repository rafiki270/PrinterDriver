#include "printerdriver/dsl/json.hpp"

#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

#include "printerdriver/dsl/text.hpp"

namespace pd::dsl {
namespace {

const std::string& emptyString() {
  static const std::string empty;
  return empty;
}

const Json::Array& emptyArray() {
  static const Json::Array empty;
  return empty;
}

const Json::Object& emptyObject() {
  static const Json::Object empty;
  return empty;
}

// Locale-independent: std::strtod honours LC_NUMERIC, and a receipt renderer that
// parses "1.5" differently on a Czech host would be exactly the class of bug this
// library exists to prevent.
double parseDoubleLiteral(const std::string& literal) {
  std::istringstream stream(literal);
  stream.imbue(std::locale::classic());
  double value = 0.0;
  stream >> value;
  if (!stream) {
    return 0.0;
  }
  return value;
}

std::string formatDouble(double value) {
  if (!std::isfinite(value)) {
    return "0";
  }
  if (value == static_cast<double>(static_cast<long long>(value)) &&
      std::fabs(value) < 1e15) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << static_cast<long long>(value);
    return stream.str();
  }
  // Shortest representation that reads back identically.
  for (int precision = 1; precision <= 17; ++precision) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(precision);
    stream << value;
    const std::string candidate = stream.str();
    if (parseDoubleLiteral(candidate) == value) {
      return candidate;
    }
  }
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream.precision(17);
  stream << value;
  return stream.str();
}

class Parser {
 public:
  Parser(std::string_view text) : text_(text) {}

  Json parse() {
    skipWhitespace();
    Json value = parseValue(0);
    skipWhitespace();
    if (position_ != text_.size()) {
      fail("trailing content after the top-level value");
    }
    return value;
  }

 private:
  [[noreturn]] void fail(const std::string& message) const {
    throw JsonError("JSON at byte " + std::to_string(position_) + ": " + message);
  }

  bool done() const noexcept { return position_ >= text_.size(); }
  char peek() const noexcept { return done() ? '\0' : text_[position_]; }

  void skipWhitespace() {
    while (!done()) {
      const char c = text_[position_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  void expect(char c) {
    if (done() || text_[position_] != c) {
      fail(std::string("expected '") + c + "'");
    }
    ++position_;
  }

  bool literal(std::string_view word) {
    if (text_.compare(position_, word.size(), word) == 0) {
      position_ += word.size();
      return true;
    }
    return false;
  }

  Json parseValue(size_t depth) {
    if (depth > kMaxJsonDepth) {
      fail("nesting deeper than " + std::to_string(kMaxJsonDepth) + " levels");
    }
    if (done()) {
      fail("unexpected end of input");
    }
    switch (peek()) {
      case '{':
        return parseObject(depth);
      case '[':
        return parseArray(depth);
      case '"':
        return Json::string(parseString());
      case 't':
        if (literal("true")) {
          return Json::boolean(true);
        }
        fail("invalid literal");
        break;
      case 'f':
        if (literal("false")) {
          return Json::boolean(false);
        }
        fail("invalid literal");
        break;
      case 'n':
        if (literal("null")) {
          return Json::null();
        }
        fail("invalid literal");
        break;
      default:
        break;
    }
    return parseNumber();
  }

  Json parseObject(size_t depth) {
    expect('{');
    Json::Object members;
    skipWhitespace();
    if (peek() == '}') {
      ++position_;
      return Json::object(std::move(members));
    }
    while (true) {
      skipWhitespace();
      if (peek() != '"') {
        fail("object keys must be quoted strings");
      }
      std::string key = parseString();
      skipWhitespace();
      expect(':');
      skipWhitespace();
      Json value = parseValue(depth + 1);
      // Last duplicate wins, which is what every mainstream JSON reader does.
      bool replaced = false;
      for (Json::Member& member : members) {
        if (member.first == key) {
          member.second = std::move(value);
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        members.emplace_back(std::move(key), std::move(value));
      }
      skipWhitespace();
      if (peek() == ',') {
        ++position_;
        skipWhitespace();
        if (peek() == '}') {
          fail("trailing comma in object");
        }
        continue;
      }
      expect('}');
      break;
    }
    return Json::object(std::move(members));
  }

  Json parseArray(size_t depth) {
    expect('[');
    Json::Array values;
    skipWhitespace();
    if (peek() == ']') {
      ++position_;
      return Json::array(std::move(values));
    }
    while (true) {
      skipWhitespace();
      values.push_back(parseValue(depth + 1));
      skipWhitespace();
      if (peek() == ',') {
        ++position_;
        skipWhitespace();
        if (peek() == ']') {
          fail("trailing comma in array");
        }
        continue;
      }
      expect(']');
      break;
    }
    return Json::array(std::move(values));
  }

  uint32_t parseHex4() {
    if (position_ + 4 > text_.size()) {
      fail("truncated \\u escape");
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_ + static_cast<size_t>(i)];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        fail("invalid hex digit in \\u escape");
      }
    }
    position_ += 4;
    return value;
  }

  std::string parseString() {
    expect('"');
    std::string out;
    while (true) {
      if (done()) {
        fail("unterminated string");
      }
      const auto byte = static_cast<unsigned char>(text_[position_]);
      if (byte == '"') {
        ++position_;
        break;
      }
      if (byte < 0x20) {
        fail("raw control character in string");
      }
      if (byte != '\\') {
        out.push_back(text_[position_]);
        ++position_;
        continue;
      }
      ++position_;
      if (done()) {
        fail("unterminated escape");
      }
      const char escape = text_[position_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t codepoint = parseHex4();
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            // High surrogate: a low surrogate must follow, or the escape is malformed.
            if (position_ + 1 < text_.size() && text_[position_] == '\\' &&
                text_[position_ + 1] == 'u') {
              position_ += 2;
              const uint32_t low = parseHex4();
              if (low < 0xDC00 || low > 0xDFFF) {
                fail("high surrogate not followed by a low surrogate");
              }
              codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            } else {
              fail("unpaired high surrogate");
            }
          } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            fail("unpaired low surrogate");
          }
          out += text::encode(codepoint);
          break;
        }
        default:
          fail("unknown escape");
      }
    }
    return out;
  }

  Json parseNumber() {
    const size_t start = position_;
    if (peek() == '-') {
      ++position_;
    }
    if (done()) {
      fail("truncated number");
    }
    if (peek() == '0') {
      ++position_;
      if (!done() && peek() >= '0' && peek() <= '9') {
        fail("leading zeros are not valid JSON");
      }
    } else if (peek() >= '1' && peek() <= '9') {
      while (!done() && peek() >= '0' && peek() <= '9') {
        ++position_;
      }
    } else {
      fail("not a value");
    }
    if (!done() && peek() == '.') {
      ++position_;
      if (done() || peek() < '0' || peek() > '9') {
        fail("digit expected after the decimal point");
      }
      while (!done() && peek() >= '0' && peek() <= '9') {
        ++position_;
      }
    }
    if (!done() && (peek() == 'e' || peek() == 'E')) {
      ++position_;
      if (!done() && (peek() == '+' || peek() == '-')) {
        ++position_;
      }
      if (done() || peek() < '0' || peek() > '9') {
        fail("digit expected in the exponent");
      }
      while (!done() && peek() >= '0' && peek() <= '9') {
        ++position_;
      }
    }
    std::string text(text_.substr(start, position_ - start));
    const double value = parseDoubleLiteral(text);
    return Json::number(value, std::move(text));
  }

  std::string_view text_;
  size_t position_ = 0;
};

void escapeInto(const std::string& value, std::string* out) {
  out->push_back('"');
  for (const char raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (raw) {
      case '"': *out += "\\\""; continue;
      case '\\': *out += "\\\\"; continue;
      case '\b': *out += "\\b"; continue;
      case '\f': *out += "\\f"; continue;
      case '\n': *out += "\\n"; continue;
      case '\r': *out += "\\r"; continue;
      case '\t': *out += "\\t"; continue;
      default: break;
    }
    if (byte < 0x20) {
      char buffer[7];
      std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(byte));
      *out += buffer;
      continue;
    }
    out->push_back(raw);
  }
  out->push_back('"');
}

void writeInto(const Json& value, bool pretty, size_t indent, std::string* out) {
  const std::string pad = pretty ? std::string(indent * 2, ' ') : std::string();
  const std::string inner_pad = pretty ? std::string((indent + 1) * 2, ' ') : std::string();
  switch (value.type()) {
    case Json::Type::Null:
      *out += "null";
      return;
    case Json::Type::Bool:
      *out += value.asBool() ? "true" : "false";
      return;
    case Json::Type::Number:
      *out += value.literal().empty() ? formatDouble(value.asNumber()) : value.literal();
      return;
    case Json::Type::String:
      escapeInto(value.asString(), out);
      return;
    case Json::Type::Array: {
      const Json::Array& items = value.asArray();
      if (items.empty()) {
        *out += "[]";
        return;
      }
      *out += '[';
      for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
          *out += ',';
        }
        if (pretty) {
          *out += '\n';
          *out += inner_pad;
        }
        writeInto(items[i], pretty, indent + 1, out);
      }
      if (pretty) {
        *out += '\n';
        *out += pad;
      }
      *out += ']';
      return;
    }
    case Json::Type::Object: {
      const Json::Object& members = value.asObject();
      if (members.empty()) {
        *out += "{}";
        return;
      }
      *out += '{';
      for (size_t i = 0; i < members.size(); ++i) {
        if (i != 0) {
          *out += ',';
        }
        if (pretty) {
          *out += '\n';
          *out += inner_pad;
        }
        escapeInto(members[i].first, out);
        *out += pretty ? ": " : ":";
        writeInto(members[i].second, pretty, indent + 1, out);
      }
      if (pretty) {
        *out += '\n';
        *out += pad;
      }
      *out += '}';
      return;
    }
  }
}

}  // namespace

Json Json::null() { return Json(); }

Json Json::boolean(bool value) {
  Json json;
  json.type_ = Type::Bool;
  json.bool_ = value;
  return json;
}

Json Json::number(double value) {
  Json json;
  json.type_ = Type::Number;
  json.number_ = value;
  return json;
}

Json Json::number(double value, std::string literal) {
  Json json;
  json.type_ = Type::Number;
  json.number_ = value;
  json.literal_ = std::move(literal);
  return json;
}

Json Json::string(std::string value) {
  Json json;
  json.type_ = Type::String;
  json.string_ = std::move(value);
  return json;
}

Json Json::array(Array values) {
  Json json;
  json.type_ = Type::Array;
  json.array_ = std::move(values);
  return json;
}

Json Json::object(Object members) {
  Json json;
  json.type_ = Type::Object;
  json.object_ = std::move(members);
  return json;
}

bool Json::asBool(bool fallback) const noexcept {
  return type_ == Type::Bool ? bool_ : fallback;
}

double Json::asNumber(double fallback) const noexcept {
  return type_ == Type::Number ? number_ : fallback;
}

long long Json::asInt(long long fallback) const noexcept {
  if (type_ != Type::Number) {
    return fallback;
  }
  if (!std::isfinite(number_)) {
    return fallback;
  }
  return static_cast<long long>(number_ < 0 ? number_ - 0.5 : number_ + 0.5);
}

const std::string& Json::asString() const noexcept {
  return type_ == Type::String ? string_ : emptyString();
}

const Json::Array& Json::asArray() const noexcept {
  return type_ == Type::Array ? array_ : emptyArray();
}

const Json::Object& Json::asObject() const noexcept {
  return type_ == Type::Object ? object_ : emptyObject();
}

const Json* Json::find(std::string_view key) const noexcept {
  if (type_ != Type::Object) {
    return nullptr;
  }
  for (const Member& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

const Json* Json::at(size_t index) const noexcept {
  if (type_ != Type::Array || index >= array_.size()) {
    return nullptr;
  }
  return &array_[index];
}

size_t Json::size() const noexcept {
  if (type_ == Type::Array) {
    return array_.size();
  }
  if (type_ == Type::Object) {
    return object_.size();
  }
  return 0;
}

bool Json::truthy() const noexcept {
  switch (type_) {
    case Type::Null:
      return false;
    case Type::Bool:
      return bool_;
    case Type::Number:
      return number_ != 0.0;
    case Type::String:
      return !string_.empty();
    case Type::Array:
      return !array_.empty();
    case Type::Object:
      return !object_.empty();
  }
  return false;
}

void Json::set(std::string key, Json value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    array_.clear();
    string_.clear();
    literal_.clear();
    object_.clear();
  }
  for (Member& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

void Json::push(Json value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    object_.clear();
    string_.clear();
    literal_.clear();
    array_.clear();
  }
  array_.push_back(std::move(value));
}

void Json::remove(std::string_view key) {
  if (type_ != Type::Object) {
    return;
  }
  for (size_t i = 0; i < object_.size(); ++i) {
    if (object_[i].first == key) {
      object_.erase(object_.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

bool Json::operator==(const Json& other) const noexcept {
  if (type_ != other.type_) {
    return false;
  }
  switch (type_) {
    case Type::Null:
      return true;
    case Type::Bool:
      return bool_ == other.bool_;
    case Type::Number:
      return number_ == other.number_;
    case Type::String:
      return string_ == other.string_;
    case Type::Array:
      return array_ == other.array_;
    case Type::Object:
      return object_ == other.object_;
  }
  return false;
}

Json parseJson(std::string_view text) {
  Parser parser(text);
  return parser.parse();
}

bool tryParseJson(std::string_view text, Json* out, std::string* error) {
  try {
    Json value = parseJson(text);
    if (out != nullptr) {
      *out = std::move(value);
    }
    return true;
  } catch (const JsonError& failure) {
    if (error != nullptr) {
      *error = failure.what();
    }
    return false;
  }
}

std::string toJson(const Json& value, bool pretty) {
  std::string out;
  writeInto(value, pretty, 0, &out);
  return out;
}

}  // namespace pd::dsl
