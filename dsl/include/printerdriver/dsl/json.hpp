#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A strict JSON subset, written by hand because the core has no third-party
// dependencies and that rule applies to the document tier too
// (docs/receipt-dsl.md: "the canonical form is a plain data model (JSON-representable)").
//
// Strict means RFC 8259 and nothing else: no comments, no trailing commas, no single
// quotes, no unquoted keys, no NaN/Infinity, no leading '+' or '.', no leading zeros,
// no raw control characters inside strings, and no trailing garbage after the top-level
// value. A template that parses here parses in every other JSON reader on the planet,
// which is the whole point of a wire format.
//
// Two deliberate properties:
//
//   * Object members keep their insertion order, so parse → serialize is stable and a
//     stored template does not churn in version control.
//   * Number literals are kept verbatim. "1234.50" round-trips as "1234.50" rather than
//     as whatever the shortest double formatting happens to produce, and money values
//     printed without a formatter keep the digits the model author wrote.

namespace pd::dsl {

class JsonError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  using Array = std::vector<Json>;
  using Member = std::pair<std::string, Json>;
  using Object = std::vector<Member>;

  Json() = default;

  static Json null();
  static Json boolean(bool value);
  static Json number(double value);
  // The literal is what serialization emits; used by the parser to keep digits exact.
  static Json number(double value, std::string literal);
  static Json string(std::string value);
  static Json array(Array values);
  static Json object(Object members);

  Type type() const noexcept { return type_; }
  bool isNull() const noexcept { return type_ == Type::Null; }
  bool isBool() const noexcept { return type_ == Type::Bool; }
  bool isNumber() const noexcept { return type_ == Type::Number; }
  bool isString() const noexcept { return type_ == Type::String; }
  bool isArray() const noexcept { return type_ == Type::Array; }
  bool isObject() const noexcept { return type_ == Type::Object; }

  bool asBool(bool fallback = false) const noexcept;
  double asNumber(double fallback = 0.0) const noexcept;
  long long asInt(long long fallback = 0) const noexcept;
  const std::string& asString() const noexcept;   // empty when not a string
  const std::string& literal() const noexcept { return literal_; }
  const Array& asArray() const noexcept;          // empty when not an array
  const Object& asObject() const noexcept;        // empty when not an object

  // Object member lookup, nullptr when absent or when this is not an object.
  const Json* find(std::string_view key) const noexcept;
  // Array element, nullptr when out of range or when this is not an array.
  const Json* at(size_t index) const noexcept;
  size_t size() const noexcept;

  // docs/receipt-dsl.md `if`/`unless` truthiness: null, false, 0, "", [] and {} are
  // false; everything else is true.
  bool truthy() const noexcept;

  // Builders. set() on a non-object and push() on a non-array convert this value.
  void set(std::string key, Json value);
  void push(Json value);
  void remove(std::string_view key);

  bool operator==(const Json& other) const noexcept;
  bool operator!=(const Json& other) const noexcept { return !(*this == other); }

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string literal_;  // number literal as written
  std::string string_;
  Array array_;
  Object object_;
};

// Nesting deeper than this is rejected rather than recursed into: a fuzzed "[[[[[..."
// must produce an error, not a stack overflow.
inline constexpr size_t kMaxJsonDepth = 64;

// Throws JsonError, whose message carries the byte offset.
Json parseJson(std::string_view text);
// Same parser, no exception. Returns false and fills `error` on malformed input.
bool tryParseJson(std::string_view text, Json* out, std::string* error);

// Compact by default; `pretty` indents by two spaces. UTF-8 passes through unescaped,
// '/' is never escaped, control characters become \u00XX.
std::string toJson(const Json& value, bool pretty = false);

}  // namespace pd::dsl
