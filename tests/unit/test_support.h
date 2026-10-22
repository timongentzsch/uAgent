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
#include <thread>
#include <utility>
#include <vector>

#include "include/api/client.h"
#include "include/core/fs.h"
#include "include/core/settings_store.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {

// Points `api` at `url` as the address declares that endpoint, and starts its
// capabilities afresh.
inline void UseEndpoint(Api& api, const std::string& url,
                        WireApi wire_api = WireApi::kChatCompletions,
                        bool hosted_search = false) {
  api.base_url = url;
  api.wire_api = wire_api;
  api.features = {};
  api.options = nullptr;
  DeclareEndpoint(api, {});
  if (hosted_search) api.features.Add(Feature::kHostedSearch);
  api.capabilities = CapabilitiesForRoute(api.wire_api, api.features);
}

// What OpenRouter declares of itself, at whatever address `api` is on.
inline void UseOpenRouterDialect(Api& api) {
  const std::string url = api.base_url;
  UseEndpoint(api, "https://openrouter.ai/api/v1");
  api.base_url = url;
}

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

// Polls `done` until it holds or `budget_ms` passes; returns whether it held.
// Only for state that offers nothing to block on; pass a BudgetMs() deadline.
template <typename Predicate>
bool WaitUntil(Predicate done, int64_t budget_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// Writes `text` to `path`, creating its directories first.
inline void WriteFile(const std::filesystem::path& path,
                      const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
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
    // Named after this home and kept in /tmp, so it is not under `root`.
    std::filesystem::remove_all(RuntimeDir(), ignored);
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

// Check for a tool call that should have succeeded: a failure also prints what
// the tool said, which is the only account of why.
inline bool CheckOk(const ToolResult& result, const char* expression,
                    const char* file, int line) {
  if (Check(result.Ok(), expression, file, line)) return true;
  std::cerr << "  " << result.output << '\n';
  return false;
}

// The schema error a tool call would get, or empty when the arguments fit.
inline std::string InvalidToolArgument(const Tool& tool, const json& args) {
  auto issue = FindToolArgumentIssue(tool, args);
  return issue ? issue->message : std::string();
}

// Saves `values` for all conversations, or for `folder`'s. Returns why not.
inline std::string SaveSettings(const SettingValues& values,
                                const std::string& folder = "") {
  return ChangeSettings(folder, [&](SettingValues& scope) {
    for (const auto& [name, value] : values) scope[name] = value;
    return std::string();
  });
}

}  // namespace uagent

#define CHECK(expression) \
  ::uagent::Check((expression), #expression, __FILE__, __LINE__)

#define CHECK_OK(result) \
  ::uagent::CheckOk((result), #result, __FILE__, __LINE__)

// CHECK for a condition the rest of the test body dereferences: a failed CHECK
// keeps going, so `CHECK(opt.has_value()); use(*opt);` crashes the whole binary
// instead of reporting one failed case. REQUIRE reports and leaves.
#define REQUIRE(expression)                                              \
  do {                                                                   \
    if (!::uagent::Check((expression), #expression, __FILE__, __LINE__)) \
      return;                                                            \
  } while (0)

#endif  // UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
