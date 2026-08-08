#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

// Minimal assert harness. No external test framework: the core has no third-party
// dependencies, and that rule applies to its tests too.

namespace pdtest {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();
int registerTest(const char* name, void (*fn)());
void recordFailure(const char* file, int line, const std::string& message);
void resetFailures();
int failureCount();

std::string hex(const std::vector<uint8_t>& bytes);

template <typename T>
std::string show(const T& value) {
  std::ostringstream os;
  if constexpr (std::is_same_v<T, bool>) {
    os << (value ? "true" : "false");
  } else if constexpr (std::is_enum_v<T>) {
    os << static_cast<long long>(static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_integral_v<T>) {
    os << static_cast<long long>(value) << " (0x" << std::hex << std::uppercase
       << static_cast<long long>(value) << ")";
  } else {
    os << value;
  }
  return os.str();
}

void checkBytes(const char* file, int line, const char* expression,
                const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected);

}  // namespace pdtest

#define PD_TEST(test_name)                                                     \
  static void test_name();                                                     \
  static const int pd_registration_##test_name =                               \
      ::pdtest::registerTest(#test_name, &test_name);                          \
  static void test_name()

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      ::pdtest::recordFailure(__FILE__, __LINE__,                              \
                              std::string("CHECK failed: ") + #expression);    \
    }                                                                          \
  } while (false)

#define CHECK_EQ(actual, expected)                                             \
  do {                                                                         \
    const auto pd_actual = (actual);                                           \
    const auto pd_expected = (expected);                                       \
    if (!(pd_actual == pd_expected)) {                                         \
      ::pdtest::recordFailure(                                                 \
          __FILE__, __LINE__,                                                  \
          std::string("CHECK_EQ failed: ") + #actual + "\n    actual:   " +    \
              ::pdtest::show(pd_actual) + "\n    expected: " +                 \
              ::pdtest::show(pd_expected));                                    \
    }                                                                          \
  } while (false)

#define CHECK_THROWS(expression, exception_type)                               \
  do {                                                                         \
    bool pd_threw = false;                                                     \
    try {                                                                      \
      (void)(expression);                                                      \
    } catch (const exception_type&) {                                          \
      pd_threw = true;                                                         \
    } catch (...) {                                                            \
      ::pdtest::recordFailure(__FILE__, __LINE__,                              \
                              std::string("wrong exception type from: ") +     \
                                  #expression);                                \
      pd_threw = true;                                                         \
    }                                                                          \
    if (!pd_threw) {                                                           \
      ::pdtest::recordFailure(__FILE__, __LINE__,                              \
                              std::string("expected throw from: ") +           \
                                  #expression);                                \
    }                                                                          \
  } while (false)

#define CHECK_BYTES(actual, ...)                                               \
  ::pdtest::checkBytes(__FILE__, __LINE__, #actual, (actual),                  \
                       std::vector<uint8_t>{__VA_ARGS__})
