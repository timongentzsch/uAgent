// Copyright 2026 Timon Gentzsch

#include <curl/curl.h>

#include <algorithm>
#include <clocale>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/config_registry.h"
#include "include/core/sandbox.h"
#include "tests/unit/test_support.h"

namespace uagent {

int failures = 0;

bool Check(bool condition, const char* expression, const char* file, int line) {
  if (condition) return true;
  std::cerr << "FAIL " << file << ':' << line << ": " << expression << '\n';
  ++failures;
  return false;
}

struct TestCase {
  const char* name;
  void (*run)();
};

std::vector<TestCase>& Tests() {
  static std::vector<TestCase> cases;
  return cases;
}

bool RegisterTest(const char* name, void (*run)()) noexcept {
  Tests().push_back({name, run});
  return true;
}

int RunTests(int argc, char** argv) {
  // Registration follows link order; a run does not.
  std::sort(Tests().begin(), Tests().end(),
            [](const TestCase& left, const TestCase& right) {
              return std::string_view(left.name) < right.name;
            });
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

  // Display-width tests need a UTF-8 CTYPE locale; explicit names keep the run
  // hermetic under a bare C ambient.
  if (!std::setlocale(LC_CTYPE, "C.UTF-8")) {
    if (!std::setlocale(LC_CTYPE, "en_US.UTF-8")) {
      std::setlocale(LC_CTYPE, "");
    }
  }
  curl_global_init(CURL_GLOBAL_DEFAULT);
  size_t selected = 0;
  for (const TestCase& test : Tests()) {
    const std::string_view name = test.name;
    if (!exact.empty() && name != exact) continue;
    if (!match.empty() && name.find(match) == std::string_view::npos) continue;
    ++selected;
    std::cerr << "[ run ] " << test.name << '\n';
    const auto started = std::chrono::steady_clock::now();
    ClearSettings();
    test.run();
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::cerr << "[ done ] " << test.name << " (" << elapsed << "s)\n";
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

int main(int argc, char** argv) {
  // Linux re-execs the calling binary as the Landlock trampoline, so this one
  // needs the same dispatch as src/main.cc.
  if (argc > 2 && std::string(argv[1]) == "--sandbox-child") {
    return uagent::SandboxChildMain(argc, argv);
  }
  // As src/main.cc: a peer that has gone is an error to report, not a death.
  signal(SIGPIPE, SIG_IGN);
  return uagent::RunTests(argc, argv);
}
