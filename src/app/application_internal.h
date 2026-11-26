// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_APPLICATION_INTERNAL_H_
#define UAGENT_SRC_APP_APPLICATION_INTERNAL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "include/agent/agent.h"
#include "include/app/bootstrap.h"
#include "include/app/commands.h"
#include "include/app/runtime.h"
#include "include/core/json.h"
#include "include/media/attachments.h"

namespace uagent {

// Session runtime: turn execution, persistence, config reload and the
// headless and channel entry points.
class Application {
 public:
  explicit Application(AppContext& context);
  ~Application();
  AppSession Session();
  int Run();

 private:
  int LoadInitialAttachments();
  void ReloadConfigAtTurnBoundary();
  // What `logs.level` records from now on.
  void ApplyLogLevel();
  void RunTurns(const std::string& input, json content = nullptr,
                const json& images = json::array());
  // Returns the message the person's answer starts the next turn with.
  std::string PresentPlan();
  void LogSessionEnd(const char* reason) const;
  void Teardown(const char* reason);
  int FinishHeadless(std::string answer, std::string error, int exit_code);
  // The session's totals now: what ReportUsage counts from.
  json UsageTotals() const;
  // What the session spent since `used_before_`, appended to `ledger` for the
  // parent that drains it: an agent reports each turn.
  json ReportUsage(const std::string& ledger);
  int RunHeadless();
  // Empty, or why the named conversation could not be continued.
  std::string ResumeAtStartup();
  void SaveSession(bool force = false);
  void EnsureSessionPath();
  void ReportReplacedExecutable();
  void RunPrompt(const std::string& input);
  json InterfaceState() const;
  void ProcessInput(std::string input);
  // Runs the slash commands a person approved for the agent, after the
  // messages queued for after the turn. An incomplete turn drops them.
  void RunQueuedCommands();
  int RunChannel();
  void PublishChannelState(bool checkpoint = true);
  AppContext& context_;
  AppRuntime& runtime_;
  Api& api_;
  Agent& agent_;
  std::vector<Attachment> attachments_;
  std::string session_file_;
  uint64_t saved_revision_;
  bool persist_ = false;
  // What the session had used when this run began or last reported.
  json used_before_;
  // The settings problem last reported, so that one is said once.
  std::string config_problem_;
  // Whether search was last found unable to run for want of a model.
  bool search_unnamed_ = false;
  std::string request_id_;
  json origin_;
  bool report_ = false;  // the turn under way reports its answer
  uint64_t message_subscription_ = 0;
  // The to-do list clients were last sent.
  json todo_ = json::array();
  json suggestion_ = nullptr;
  std::string input_error_;
  ApplicationChannel* channel_ = nullptr;
};

}  // namespace uagent

#endif  // UAGENT_SRC_APP_APPLICATION_INTERNAL_H_
