// Copyright 2026 Timon Gentzsch

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/bootstrap.h"
#include "include/app/commands.h"
#include "include/app/runtime.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/mcp/rpc.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/tools/child_agent.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/tools/process.h"
#include "include/tools/subagent.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/sessions.h"

namespace uagent {
namespace {

class Application {
 public:
  explicit Application(AppContext& context)
      : context_(context),
        runtime_(context.runtime),
        api_(runtime_.api),
        agent_(*context.agent),
        session_file_(context.channel ? context.channel->SessionPath()
                                      : CollaboratorSessionFile()),
        saved_revision_(agent_.Revision()),
        channel_(context.channel) {
    agent_.RetainExchanges(channel_ || context_.options.prompt.empty() ||
                           !session_file_.empty());
    message_subscription_ =
        context_.observability.Subscribe([this](const AppEvent& event) {
          if (event.type != "message.changed") return;
          std::string kind = JsonValue(event.data["block"], "kind", "");
          if ((kind == "user" || kind == "attachment") &&
              (persist_ || !session_file_.empty())) {
            SaveSession(true);
          }
        });
  }
  ~Application() { context_.observability.Unsubscribe(message_subscription_); }

  AppSession Session() {
    return AppSession{context_, attachments_, session_file_, saved_revision_};
  }

  int Run() {
    int attachment_status = LoadInitialAttachments();
    if (attachment_status != 0) return attachment_status;
    if (!context_.options.prompt.empty() &&
        (context_.options.resume_latest || !session_file_.empty())) {
      if (!ResumeAtStartup()) return 2;
    }
    if (!context_.options.prompt.empty()) return RunHeadless();
    return channel_ ? RunChannel() : 2;
  }

 private:
  int LoadInitialAttachments() {
    for (const std::string& path : context_.options.attach_paths) {
      Attachment attachment;
      std::string error;
      if (!InspectAttachment(path, attachment, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
      }
      attachments_.push_back(std::move(attachment));
    }
    return 0;
  }

  void ReloadConfigAtTurnBoundary() {
    std::optional<ConfigReload> reload =
        context_.config_manager.Reload(runtime_.config);
    if (!reload) return;
    runtime_.config = reload->active;
    api_.config = reload->active;
    Emit(Event{EventId::kConfigChanged,
               {{"changed", reload->applied},
                {"deferred", reload->deferred},
                {"source", "config_file"}}});
    if (context_.options.prompt.empty() &&
        (!reload->applied.empty() || !reload->deferred.empty())) {
      std::string notice = "· configuration reloaded for the next turn";
      if (!reload->deferred.empty()) {
        if (reload->deferred.size() == 1) {
          notice += " · 1 setting requires restart";
        } else {
          notice += " · " + std::to_string(reload->deferred.size()) +
                    " settings require restart";
        }
      }
      Emit(NoticeEvent(PresentationStatus::kNeutral, notice));
    }
  }

  void RunTurns(const std::string& input, json content = nullptr,
                json images = json::array()) {
    EnsureSessionPath();
    ReloadConfigAtTurnBoundary();
    // Delegated handoffs inherit the parent's current remainder. Restored
    // usage belongs to the worker's cumulative limit, not to a new allowance.
    const double cost = JsonValue(handoff_budget_, "cost", 0.0);
    const int64_t tokens = JsonValue(handoff_budget_, "tokens", int64_t{0});
    if (cost > 0) {
      const double ceiling = api_.session_cost + cost;
      api_.config.session_budget =
          api_.config.session_budget > 0
              ? std::min(api_.config.session_budget, ceiling)
              : ceiling;
    }
    if (tokens > 0) {
      const int64_t ceiling =
          SaturatingNonnegativeAdd(api_.session_generated_tokens, tokens);
      api_.config.session_token_budget =
          api_.config.session_token_budget > 0
              ? std::min(api_.config.session_token_budget, ceiling)
              : ceiling;
    }
    bool was_automatic = ApprovalIsAutomatic();
    PermissionControl(context_, json::object());
    if (was_automatic != ApprovalIsAutomatic()) agent_.ApprovalChanged();
    struct TurnGuard {
      bool& flag_;
      explicit TurnGuard(bool& flag) : flag_(flag) { flag_ = true; }
      ~TurnGuard() { flag_ = false; }
    };
    // Publish lifecycle boundaries even when no model response is produced.
    {
      TurnGuard guard(turn_active_);
      if (channel_ || !session_file_.empty()) PublishChannelState(false);
      agent_.Turn(input, std::move(content), std::move(images), request_id_);
      SteeringState().Take();
    }
    if (channel_ || !session_file_.empty()) PublishChannelState(false);
  }

  void LogSessionEnd(const char* reason) const {
    Emit(Event{EventId::kSessionEnded,
               {{"reason", reason},
                {"usage", UsageJson(agent_.SessionUsage())},
                {"context_tokens", agent_.ContextUsed()}}});
  }

  // Release owned processes and per-session files, then drain observations.
  void Teardown(const char* reason) {
    runtime_.Shutdown();
    LogSessionEnd(reason);
    std::remove(UsageLedger().c_str());
    if (!session_file_.empty()) {
      std::string error;
      if (!context_.observability.Journal().Flush(
              session_file_ + ".events.jsonl", error)) {
        fprintf(stderr, "cannot save session journal: %s\n", error.c_str());
      }
    }
    context_.observability.Flush();
  }

  int FinishHeadless(std::string answer, std::string error, int exit_code) {
    Teardown(exit_code == 0 ? "headless_complete" : "headless_error");
    if (context_.options.json_stream || context_.options.json) {
      json envelope =
          HeadlessResult(std::move(answer), std::move(error),
                         agent_.LatestToolTrace(), agent_.SessionUsage(),
                         agent_.RouteUsageJson(), exit_code, agent_.LastStop());
      if (context_.options.json_stream) {
        Emit(Event{exit_code == 0 ? EventId::kAnswer : EventId::kError,
                   std::move(envelope)});
      } else {
        printf("%s\n", JsonDump(envelope).c_str());
      }
    } else if (exit_code == 0) {
      printf("%s\n", TerminalSafe(answer).c_str());
    } else {
      fprintf(stderr, "%s\n", TerminalSafe(error).c_str());
    }
    return exit_code;
  }

  int RunHeadless() {
    // Internal collaborator sessions are explicit durable conversations even
    // though ordinary one-shot `-p` calls remain ephemeral.
    persist_ = !session_file_.empty();
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(context_.options.prompt, attachments_, error);
      if (!error.empty()) {
        context_.output.Restore();
        return FinishHeadless("", std::move(error), 2);
      }
    }
    RunTurns(context_.options.prompt, std::move(content));
    SaveSession();
    PublishChannelState();
    // Background work is observational and never starts a model turn. Keep the
    // process alive long enough to publish completion and drain retained state.
    while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
      uint64_t generation = runtime_.processes.Generation();
      if (!agent_.DrainBackground()) {
        runtime_.processes.WaitForChange(generation);
      }
      SaveSession();
    }
    context_.output.Restore();

    std::string ledger = EnvStr("UAGENT_USAGE_FILE");
    if (!ledger.empty()) {
      std::string error;
      json entry = {{"route", agent_.ActiveRoute()},
                    {"routes", agent_.RouteUsageJson()},
                    {"usage", UsageJson(agent_.SessionUsage())}};
      if (!AppendPrivateLine(ledger, JsonDump(entry), error)) {
        fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
      }
    }
    std::string answer = agent_.LastText();
    if (!agent_.LastError().empty()) {
      return FinishHeadless("", agent_.LastError(), 1);
    }
    if (answer.empty()) {
      std::string error = agent_.LastError().empty()
                              ? "agent produced no answer"
                              : agent_.LastError();
      return FinishHeadless("", std::move(error), 1);
    }
    return FinishHeadless(std::move(answer), "", 0);
  }

  bool ResumeAtStartup() {
    std::string previous_path = session_file_;
    if (!session_file_.empty()) {
      if (PathExists(session_file_)) {
        if (!ResumeInto(agent_, session_file_, session_file_, !channel_)) {
          return false;
        }
      }
    } else if (context_.options.resume_pick) {
      std::string path = PickSession();
      if (!path.empty() && !ResumeInto(agent_, path, session_file_)) {
        return false;
      }
    } else if (context_.options.resume_latest) {
      std::vector<SessionInfo> sessions = ListSessions();
      if (sessions.empty()) {
        printf("%s· no saved sessions%s\n", DIM(), RST());
        fflush(stdout);
      } else {
        if (!ResumeInto(agent_, sessions.front().path, session_file_)) {
          return false;
        }
      }
    }
    AppSession session = Session();
    LoadSessionJournal(session, previous_path);
    saved_revision_ = agent_.Revision();
    return true;
  }

  void SaveSession(bool force = false) {
    auto session = Session();
    SaveSessionSettings(session);
    if ((!persist_ && session_file_.empty()) ||
        (!force && agent_.MessageCount() <= 1) ||
        (!force && agent_.Revision() == saved_revision_)) {
      return;
    }
    EnsureSessionPath();
    std::string error;
    if (!agent_.Save(session_file_, error) ||
        !context_.observability.Journal().Flush(session_file_ + ".events.jsonl",
                                                error)) {
      input_error_ = "cannot save session: " + error;
      fprintf(stderr, "%s\n", input_error_.c_str());
      Emit(Event{EventId::kError, {{"error", input_error_}}});
      return;
    }
    saved_revision_ = agent_.Revision();
  }

  void EnsureSessionPath() {
    if (persist_ && session_file_.empty()) {
      session_file_ =
          UagentDir(kHistoryDir) + "/" + WorkspaceId(CanonicalCwd()) + "/" +
          UtcStamp("%Y%m%dT%H%M%SZ") + "-" + MakeSessionId() + ".json";
    }
    runtime_.processes.SetOwner(session_file_.empty() ? agent_.SessionId()
                                                      : HashHex(session_file_));
  }

  // An improvement round ends by reinstalling, and this session is still the
  // old build: nothing it reports about itself is true of the new one.
  void ReportReplacedExecutable() {
    if (!ExecutableReplaced()) return;
    context_.observability.Emit(NoticeEvent(
        PresentationStatus::kNeutral,
        "· uagent was replaced on disk; restart to run the new build"));
  }

  void RunPrompt(const std::string& input) {
    ReportReplacedExecutable();
    input_error_.clear();
    json content;
    json images = json::array();
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(input, attachments_, error);
      if (!error.empty()) {
        input_error_ = error;
        if (!channel_) {
          printf("%s%s%s\n", RED(), TerminalSafe(error).c_str(), RST());
          fflush(stdout);
        }
        Emit(Event{EventId::kError, {{"error", error}}});
        return;
      }
      for (const auto& attachment : attachments_) {
        if (!attachment.asset_id.empty()) {
          images.push_back({{"id", attachment.asset_id},
                            {"name", attachment.name},
                            {"mime", attachment.mime},
                            {"bytes", attachment.bytes},
                            {"image", attachment.image},
                            {"path", attachment.path}});
        }
      }
      attachments_.clear();
    }
    RunTurns(input, std::move(content), std::move(images));
  }

  json InterfaceState() const {
    return {{"route", RouteSelection(api_, context_.provider.providers)},
            {"effort", api_.reasoning_effort},
            {"variant", api_.config.openrouter_variant},
            {"context_tokens", agent_.ContextUsed()},
            {"context_window", api_.ctx_window},
            {"attachments", attachments_.size()},
            {"background", runtime_.processes.Count()},
            {"tools", context_.tools.size()},
            {"verbose", agent_.Verbose()},
            {"yolo", ApprovalIsAutomatic()}};
  }

  bool ProcessInput(std::string input) {
    input = Trim(input);
    if (input.empty()) {
      if (!attachments_.empty()) RunPrompt(input);
      return false;
    }
    ParsedSlashCommand command = ParseSlashCommand(input);
    if (command.spec) DebugLog("command", {{"command", command.spec->name}});
    if (std::string prompt = SlashCommandPrompt(command); !prompt.empty()) {
      RunPrompt(prompt);
      return false;
    }
    if (command.spec) {
      AppSession session = Session();
      json result;
      // Existing slash commands produce text and structured results. Capture
      // their text once at the application boundary so every client receives
      // the same command.completed event.
      FILE* captured = channel_ ? tmpfile() : nullptr;
      fflush(stdout);
      Fd saved(captured ? dup(STDOUT_FILENO) : -1);
      if (saved) dup2(fileno(captured), STDOUT_FILENO);
      bool quit = RunSlashCommand(session, command, result);
      std::string output;
      if (captured) {
        fflush(stdout);
        if (saved) dup2(saved.Get(), STDOUT_FILENO);
        if (fseek(captured, 0, SEEK_SET) == 0) {
          output.resize(size_t{64} * 1024);
          output.resize(fread(output.data(), 1, output.size(), captured));
        } else {
          output = "cannot read command output";
        }
        fclose(captured);
      }
      output = Trim(output);
      Emit(Event{EventId::kCommandCompleted,
                 {{"command", command.spec->name},
                  {"argument", command.argument},
                  {"inspect", command.spec->inspect_result},
                  {"output", std::move(output)},
                  {"quit", quit},
                  {"result", std::move(result)},
                  {"state", InterfaceState()}}});
      if (!quit) return false;
      exit_reason_ = "command";
      return true;
    }
    if (input[0] == '/') {
      Emit(NoticeEvent(PresentationStatus::kFailed,
                       "· unknown command " + input + "; use /help"));
      return false;
    }
    RunPrompt(input);
    return false;
  }

  int FinishInteractive(int status) {
    SaveSession();
    Teardown(exit_reason_.c_str());
    TerminalRestore();
    return status;
  }

  int RunChannel() {
    if (!ResumeAtStartup()) return FinishInteractive(2);
    persist_ = true;
    EnsureSessionPath();
    if (!PathExists(session_file_) && !channel_->InitialTitle().empty()) {
      agent_.Rename(channel_->InitialTitle());
    }
    SaveSession(true);
    if (AgentDepth() == 0 && api_.config.memory_enabled &&
        api_.config.memory_generate) {
      std::string error = StartMemoryExtractor(
          runtime_.processes, api_, CanonicalAccessPath(CanonicalCwd()),
          session_file_);
      if (!error.empty()) {
        DebugLog("memory_extract_start_error", {{"error", error}});
      }
    }
    runtime_.processes.SetNotifyFd(channel_->WakeFd());
    channel_->SetActivityControl([this](const json& request) {
      if (JsonValue(request, "kind", "") == "permissions") {
        return PermissionControl(context_, request);
      }
      return ActivityControl(runtime_.processes, request,
                             &runtime_.collaborator);
    });
    PublishChannelState();
    while (std::optional<ApplicationInput> input = channel_->NextInput()) {
      agent_.DrainBackground();
      bool quit = false;
      request_id_ = input->request_id;
      if (input->title) {
        agent_.Rename(std::move(*input->title));
      } else if (!input->control.is_null()) {
        AppSession session = Session();
        json result = SessionControl(session, input->control);
        SaveSession(true);
        channel_->CompleteControl(input->request_id, result);
      } else if (!input->wake) {
        handoff_budget_ = std::move(input->budget);
        for (auto& attachment : input->attachments) {
          attachments_.push_back(std::move(attachment));
        }
        quit = ProcessInput(std::move(input->text));
      }
      SaveSession(input->title.has_value());
      PublishChannelState();
      if (quit) break;
    }
    runtime_.processes.SetNotifyFd(-1);
    channel_->SetActivityControl({});
    return FinishInteractive(0);
  }

  void PublishChannelState(bool checkpoint = true) {
    json state = InterfaceState();
    state["view"] = agent_.DisplaySnapshot();
    state["usage"] = UsageJson(agent_.SessionUsage());
    state["route_usage"] = agent_.RouteUsageJson();
    state["system_prompt"] = agent_.LastSentPrompt();
    state["statistics"] = agent_.Statistics();
    state["http"] = agent_.HttpExchanges();
    state["permissions"] = PermissionControl(context_, json::object());
    state["efforts"] = json::array({"default"});
    for (const char* effort : kReasoningEfforts) {
      if (SupportsReasoningEffort(api_, effort)) {
        state["efforts"].push_back(effort);
      }
    }
    state["variants"] = json::array();
    if (api_.capabilities.model_variants) {
      state["variants"].push_back("default");
      for (std::string_view variant : kOpenRouterVariants) {
        state["variants"].push_back(variant);
      }
    }
    state["turns"] = agent_.UserTurns();
    state["turn_active"] = turn_active_;
    state["activities"] = runtime_.processes.ActivityViews();
    state["collaborators"] =
        CollaboratorSummaries(runtime_.processes, &runtime_.collaborator);
    state["error"] = input_error_.empty() ? agent_.LastError() : input_error_;
    state["title"] = Utf8Prefix(agent_.FirstUserText(), 256);
    state["stop"] = agent_.LastStop();
    if (channel_) channel_->PublishState(state, checkpoint);
  }

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

}  // namespace

int RunApplication(AppContext& context) { return Application(context).Run(); }

}  // namespace uagent
