// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
#define UAGENT_TESTS_UNIT_TEST_SUPPORT_H_

#include <unistd.h>

#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace uagent {

// Scale a millisecond deadline for an instrumented build, mirroring the Python
// harness's budget(). A sanitizer leg starts and renders several times slower
// than a plain one, so a fixed PTY round-trip budget there is a coin flip; CI
// raises the multiplier for those jobs rather than each deadline being retuned
// by hand.
inline int64_t BudgetMs(int64_t milliseconds) {
  const char* scale = std::getenv("UAGENT_TEST_TIMEOUT_SCALE");
  if (scale == nullptr || milliseconds <= 0) return milliseconds;
  char* end = nullptr;
  const double factor = std::strtod(scale, &end);
  const double scaled = static_cast<double>(milliseconds) * factor;
  if (end == scale || *end != '\0' || !std::isfinite(factor) || factor <= 1.0 ||
      !std::isfinite(scaled) ||
      scaled >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    return milliseconds;
  }
  return static_cast<int64_t>(scaled);
}

class TestWorkspace {
 public:
  explicit TestWorkspace(const std::string& name)
      : root(
            std::filesystem::temp_directory_path() /
            ("uagent-" + name + "-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        workspace(root / "workspace"),
        home(root / "home"),
        original_(std::filesystem::current_path()) {
    const char* value = std::getenv("HOME");
    had_home_ = value != nullptr;
    if (value) prior_home_ = value;
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    std::filesystem::current_path(workspace);
    workspace = std::filesystem::current_path();
  }

  ~TestWorkspace() {
    std::error_code ignored;
    std::filesystem::current_path(original_, ignored);
    if (had_home_) {
      setenv("HOME", prior_home_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(root, ignored);
  }

  TestWorkspace(const TestWorkspace&) = delete;
  TestWorkspace& operator=(const TestWorkspace&) = delete;

  std::filesystem::path root;
  std::filesystem::path workspace;
  std::filesystem::path home;

 private:
  std::filesystem::path original_;
  std::string prior_home_;
  bool had_home_ = false;
};

// Sets an environment variable immediately and restores the inherited value
// (or its absence) when the scope ends, so a test may read back what it set.
class ScopedEnv {
 public:
  ScopedEnv(const char* key, const char* value) : key_(key) {
    const char* prior = std::getenv(key);
    had_ = prior != nullptr;
    if (prior) prior_ = prior;
    if (value) {
      setenv(key, value, 1);
    } else {
      unsetenv(key);
    }
  }
  ScopedEnv(const char* key, const std::string& value)
      : ScopedEnv(key, value.c_str()) {}
  // Unsets for the scope.
  explicit ScopedEnv(const char* key) : ScopedEnv(key, nullptr) {}

  ~ScopedEnv() {
    if (had_) {
      setenv(key_, prior_.c_str(), 1);
    } else {
      unsetenv(key_);
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* key_;
  std::string prior_;
  bool had_ = false;
};

extern int failures;
// Records a failure and returns whether the condition held; the run continues
// so one execution reports every failure rather than the first.
bool Check(bool condition, const char* expression, const char* file, int line);

}  // namespace uagent

#define CHECK(expression) \
  ::uagent::Check((expression), #expression, __FILE__, __LINE__)

// CHECK for a condition the rest of the test body dereferences: a failed CHECK
// keeps going, so `CHECK(opt.has_value()); use(*opt);` crashes the whole binary
// instead of reporting one failed case. REQUIRE reports and leaves.
#define REQUIRE(expression)                                              \
  do {                                                                   \
    if (!::uagent::Check((expression), #expression, __FILE__, __LINE__)) \
      return;                                                            \
  } while (0)

#endif  // UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
