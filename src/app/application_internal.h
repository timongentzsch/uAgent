// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_APPLICATION_INTERNAL_H_
#define UAGENT_SRC_APP_APPLICATION_INTERNAL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "include/agent.h"
#include "include/app/bootstrap.h"
#include "include/app/commands.h"
#include "include/app/runtime.h"
#include "include/core/json.h"
#include "include/media/attachments.h"

namespace uagent {

// Session runtime: turn execution, persistence, config reload and the
// consumer entry points (headless, channel). Terminal and browser
// clients are projections over the events and state this produces.
class Application {
 public:
  explicit Application(AppContext& context);
  ~Application();
  AppSession Session();
  int Run();

 private:
  int LoadInitialAttachments();
  void ReloadConfigAtTurnBoundary();
  void RunTurns(const std::string& input, json content = nullptr,
                json images = json::array());
  void LogSessionEnd(const char* reason) const;
  void Teardown(const char* reason);
  int FinishHeadless(std::string answer, std::string error, int exit_code);
  int RunHeadless();
  bool ResumeAtStartup();
  void SaveSession(bool force = false);
  void EnsureSessionPath();
  void ReportReplacedExecutable();
  void RunPrompt(const std::string& input);
  json InterfaceState() const;
  bool ProcessInput(std::string input);
  int FinishInteractive(int status);
  int RunChannel();
  json BuildChannelState() const;
  void PublishChannelState(bool checkpoint = true);
  AppContext& context_;
  AppRuntime& runtime_;
  Api& api_;
  Agent& agent_;
  std::vector<Attachment> attachments_;
  std::string session_file_;
  uint64_t saved_revision_;
  bool persist_ = false;
  bool turn_active_ = false;
  std::string request_id_;
  uint64_t message_subscription_ = 0;
  std::string exit_reason_ = "eof";
  std::string input_error_;
  json handoff_budget_;
  ApplicationChannel* channel_ = nullptr;
};

}  // namespace uagent

#endif  // UAGENT_SRC_APP_APPLICATION_INTERNAL_H_
