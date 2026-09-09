// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_BOOTSTRAP_H_
#define UAGENT_INCLUDE_APP_BOOTSTRAP_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/options.h"
#include "include/app/runtime.h"
#include "include/core/effective_config.h"
#include "include/core/events.h"
#include "include/core/fd.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/tools/tool.h"

namespace uagent {

struct InteractionRequest;

struct ApplicationInput {
  std::string text{};
  std::string request_id{};
  json control{};
  std::vector<Attachment> attachments{};
  bool wake = false;
  std::optional<std::string> title{};
};

// Transport-neutral input half of the application protocol. A terminal, a
// JSON-RPC app server, or an in-process GUI can supply commands and answer
// interactive decisions while Observability::Subscribe supplies every output
// event. Returning nullopt closes the session.
class ApplicationChannel {
 public:
  virtual ~ApplicationChannel() = default;
  virtual std::optional<ApplicationInput> NextInput() = 0;
  virtual std::string ReadInteraction(const InteractionRequest& request,
                                      bool* eof) = 0;
  virtual int WakeFd() const { return -1; }
  virtual std::string SessionPath() const { return {}; }
  virtual std::string InitialTitle() const { return {}; }
  // Called only at serialized application boundaries, after saving.
  virtual void PublishState(const json&) {}
  virtual void CompleteControl(const std::string&, const json&) {}
  // Only thread-safe, non-model activity operations may use this entry point.
  virtual void SetActivityControl(const std::function<json(const json&)>&) {}
};

class HeadlessOutput {
 public:
  HeadlessOutput() = default;
  ~HeadlessOutput();

  HeadlessOutput(const HeadlessOutput&) = delete;
  HeadlessOutput& operator=(const HeadlessOutput&) = delete;

  bool Silence();
  void Restore();

 private:
  Fd saved_stdout_;  // the real stdout, parked while output is silenced
};

struct AppContext {
  AppContext(RuntimeConfig config, ConfigManager config_manager,
             Options parsed_options, Observability& observation_sink,
             ApplicationChannel* application_channel);
  ~AppContext();

  CurlRuntime curl;
  ConfigManager config_manager;
  AppRuntime runtime;
  Observability& observability;
  ApplicationChannel* channel;
  Options options;
  ProviderSetup provider;
  ToolPolicy tool_policy;
  std::vector<Tool> tools;
  // What "don't ask again" granted, bound to the tool's current provider,
  // schema and approval policy. Session-scoped by construction: it dies here.
  std::vector<std::string> session_approvals;
  std::atomic<int> permission_override{-1};
  std::unique_ptr<Agent> agent;
  HeadlessOutput output;
};

struct BootstrapResult {
  std::unique_ptr<AppContext> context;
  std::string error;
  int exit_code = 1;

  bool Ok() const { return context != nullptr; }
};

BootstrapResult Bootstrap(Options options, const char* executable,
                          Observability& observability,
                          ApplicationChannel* channel = nullptr);

int RunApplication(AppContext& context);

// `stop` is additive: the schema string is what consumers match on, and a
// field they do not know is cheaper for them than a version they must handle.
inline json HeadlessResult(std::string answer, std::string error, json trace,
                           const Usage& usage, json routes, int exit_code,
                           json stop = nullptr) {
  return {{"schema", "uagent.headless.v1"},
          {"answer", std::move(answer)},
          {"error", error.empty() ? json(nullptr) : json(std::move(error))},
          {"stop", stop.is_null() ? json(nullptr) : std::move(stop)},
          {"trace", std::move(trace)},
          {"usage", UsageJson(usage)},
          {"routes", std::move(routes)},
          {"exit_code", exit_code}};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_BOOTSTRAP_H_
