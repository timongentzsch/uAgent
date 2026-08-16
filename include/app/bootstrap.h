// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_BOOTSTRAP_H_
#define UAGENT_INCLUDE_APP_BOOTSTRAP_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/options.h"
#include "include/app/runtime.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/providers.h"
#include "include/tools/tool.h"

namespace uagent {

class HeadlessOutput {
 public:
  HeadlessOutput() = default;
  ~HeadlessOutput();

  HeadlessOutput(const HeadlessOutput&) = delete;
  HeadlessOutput& operator=(const HeadlessOutput&) = delete;

  bool Silence();
  void Restore();

 private:
  int saved_stdout_ = -1;
};

struct AppContext {
  AppContext(RuntimeConfig config, Options parsed_options);

  CurlRuntime curl;
  AppRuntime runtime;
  Options options;
  ProviderSetup provider;
  ToolPolicy tool_policy;
  std::vector<Tool> tools;
  std::unique_ptr<Agent> agent;
  HeadlessOutput output;
};

struct BootstrapResult {
  std::unique_ptr<AppContext> context;
  std::string error;
  int exit_code = 1;

  bool Ok() const { return context != nullptr; }
};

BootstrapResult Bootstrap(Options options, const char* executable);
int RunApplication(AppContext& context);

inline json HeadlessResult(std::string answer, std::string error, json trace,
                           const Usage& usage, json routes, int exit_code) {
  return {{"schema", "uagent.headless.v1"},
          {"answer", std::move(answer)},
          {"error", error.empty() ? json(nullptr) : json(std::move(error))},
          {"trace", std::move(trace)},
          {"usage", UsageJson(usage)},
          {"routes", std::move(routes)},
          {"exit_code", exit_code}};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_BOOTSTRAP_H_
