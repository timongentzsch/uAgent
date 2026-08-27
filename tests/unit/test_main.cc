// Copyright 2026 Timon Gentzsch

#include <curl/curl.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

int failures = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
  if (condition) return;
  std::cerr << "FAIL " << file << ':' << line << ": " << expression << '\n';
  ++failures;
}

struct TestCase {
  const char* name;
  void (*run)();
};

const std::vector<TestCase>& Tests() {
  static const std::vector<TestCase> kCases = {
#define UAGENT_TEST_CASE(name) {#name, &(name)},
      UAGENT_TESTS(UAGENT_TEST_CASE)
#undef UAGENT_TEST_CASE
  };
  return kCases;
}

int RunTests(int argc, char** argv) {
  std::string exact;
  std::string match;
  bool list = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--list") {
      list = true;
    } else if ((argument == "--test" || argument == "-k") && index + 1 < argc) {
      std::string& target = argument == "--test" ? exact : match;
      target = argv[++index];
    } else {
      std::cerr
          << "usage: uagent_tests [--list] [--test NAME] [-k SUBSTRING]\n";
      return 2;
    }
  }

  if (list) {
    for (const TestCase& test : Tests()) std::cout << test.name << '\n';
    return 0;
  }

  std::setlocale(LC_CTYPE, "");
  curl_global_init(CURL_GLOBAL_DEFAULT);
  size_t selected = 0;
  for (const TestCase& test : Tests()) {
    const std::string_view name = test.name;
    if (!exact.empty() && name != exact) continue;
    if (!match.empty() && name.find(match) == std::string_view::npos) continue;
    ++selected;
    std::cerr << "[ run ] " << test.name << '\n';
    test.run();
  }
  curl_global_cleanup();

  if (selected == 0) {
    std::cerr << "no core tests selected\n";
    return 2;
  }
  if (failures) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "all " << selected << " selected core tests passed\n";
  return 0;
}

}  // namespace uagent

int main(int argc, char** argv) { return uagent::RunTests(argc, argv); }
