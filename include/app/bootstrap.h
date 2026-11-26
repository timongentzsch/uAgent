// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_BOOTSTRAP_H_
#define UAGENT_INCLUDE_APP_BOOTSTRAP_H_
// Starting a session runtime: the input and output channels it works
// through and the application context built from the options.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/agent.h"
#include "include/app/permissions.h"
#include "include/app/runtime.h"
#include "include/cli/options.h"
#include "include/core/effective_config.h"
#include "include/core/events.h"
#include "include/core/fd.h"
#include "include/core/json.h"
#include "include/core/runtime_config.h"
#include "include/core/skills.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"

namespace uagent {

struct InteractionRequest;

struct ApplicationInput {
  std::string text{};
  std::string request_id{};
  json control{};
  std::vector<Attachment> attachments{};
  bool wake = false;
  // Who wrote the text when not the person (Steering::Message::origin).
  json origin{};
  std::optional<std::string> title{};
  // Keeps the conversation where it is (Agent::KeepHere).
  bool keep = false;
  // The turn's answer is reported to another conversation, so published
  // whole.
  bool report = false;
};

// Transport-neutral input half of the application protocol. NextInput
// returning nullopt closes the session.
class ApplicationChannel {
 public:
  virtual ~ApplicationChannel() = default;
  virtual std::optional<ApplicationInput> NextInput() = 0;
  virtual std::string ReadInteraction(const InteractionRequest& request,
                                      bool* eof) = 0;
  virtual int WakeFd() const { return -1; }
  virtual std::string SessionPath() const { return {}; }
  virtual std::string InitialTitle() const { return {}; }
  // A checkpoint follows a save; a live publish carries the state mid-turn.
  virtual void PublishState(const json&, bool /*checkpoint*/) {}
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
  // The installed skills the skill tool offers beside served ones.
  std::vector<Skill> skills;
  // The name this session holds the shared browser under.
  std::string browser_lease;
  SessionRules session_rules;
  // The sudo passwords run(root=true) was given this turn, by host ("" for
  // this machine); wiped when the turn ends.
  std::mutex root_passwords_mutex;
  std::map<std::string, std::shared_ptr<std::string>> root_passwords;
  // Slash commands a person approved for the agent, run when the turn ends.
  std::vector<std::string> commands;
  std::unique_ptr<Agent> agent;
  HeadlessOutput output;
  std::jthread remote_watch;
};

// A start refused for want of a provider: its text begins so, and it reaches
// clients with the code beside it, which offer the providers settings.
inline constexpr std::string_view kNoProviderError = "no provider: ";
inline constexpr std::string_view kNoProviderCode = "no_provider";
// The code clients key on for a start refusal's text; empty for most.
inline std::string_view RefusalCode(std::string_view error) {
  return error.starts_with(kNoProviderError) ? kNoProviderCode : "";
}

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

// `stop` is additive: consumers match on the schema string, which stays v1.
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
