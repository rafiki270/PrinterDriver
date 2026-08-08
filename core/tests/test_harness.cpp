#include "test_harness.hpp"

#include <exception>
#include <iostream>

namespace pdtest {
namespace {
int g_failures = 0;
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

int registerTest(const char* name, void (*fn)()) {
  registry().push_back(TestCase{name, fn});
  return 0;
}

void recordFailure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::cout << "    " << file << ":" << line << ": " << message << "\n";
}

void resetFailures() { g_failures = 0; }

int failureCount() { return g_failures; }

std::string hex(const std::vector<uint8_t>& bytes) {
  std::ostringstream os;
  os << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) {
      os << ' ';
    }
    os << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return os.str();
}

void checkBytes(const char* file, int line, const char* expression,
                const std::vector<uint8_t>& actual,
                const std::vector<uint8_t>& expected) {
  if (actual == expected) {
    return;
  }
  recordFailure(file, line,
                std::string("CHECK_BYTES failed: ") + expression +
                    "\n    actual:   [" + hex(actual) + "]\n    expected: [" +
                    hex(expected) + "]");
}

}  // namespace pdtest

int main() {
  int failed_tests = 0;
  for (const auto& test : pdtest::registry()) {
    pdtest::resetFailures();
    try {
      test.fn();
    } catch (const std::exception& error) {
      pdtest::recordFailure("<test body>", 0,
                            std::string("uncaught exception: ") + error.what());
    } catch (...) {
      pdtest::recordFailure("<test body>", 0, "uncaught non-standard exception");
    }
    if (pdtest::failureCount() > 0) {
      ++failed_tests;
      std::cout << "FAIL  " << test.name << "\n";
    } else {
      std::cout << "ok    " << test.name << "\n";
    }
  }
  std::cout << "\n"
            << (pdtest::registry().size() - static_cast<size_t>(failed_tests))
            << "/" << pdtest::registry().size() << " tests passed\n";
  return failed_tests == 0 ? 0 : 1;
}
