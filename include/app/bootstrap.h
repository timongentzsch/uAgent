// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_BOOTSTRAP_H_
#define UAGENT_INCLUDE_APP_BOOTSTRAP_H_

#include <memory>
#include <string>
#include <vector>

#include "include/agent.h"
#include "include/app/options.h"
#include "include/app/runtime.h"
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
  bool debug = false;
};

struct BootstrapResult {
  std::unique_ptr<AppContext> context;
  std::string error;
  int exit_code = 1;

  bool Ok() const { return context != nullptr; }
};

BootstrapResult Bootstrap(Options options, const char* executable);
int RunApplication(AppContext& context);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_BOOTSTRAP_H_
