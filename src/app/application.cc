// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/app/launch.h"
#include "include/browser/browser.h"
#include "include/cli/cli.h"
#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/media/attachments.h"
#include "include/providers/providers.h"
#include "include/tools/tool.h"
#include "include/tools/web_search.h"
#include "include/ui/sessions.h"
#include "src/app/application_internal.h"

namespace uagent {
Application::Application(AppContext& context)
    : context_(context),
      runtime_(context.runtime),
      api_(runtime_.api),
      agent_(*context.agent),
      session_file_(context.channel ? context.channel->SessionPath()
                                    : DelegatedSessionFile()),
      saved_revision_(agent_.Revision()),
      config_problem_(context.config_manager.Problem()),
      channel_(context.channel) {
  agent_.RetainExchanges(channel_ || context_.options.prompt.empty() ||
                         !session_file_.empty());
  message_subscription_ =
      context_.observability.Subscribe([this](const AppEvent& event) {
        if (event.type != AppType(EventId::kMessageChanged)) {
          return;
        }
        std::string kind = JsonValue(event.data["block"], "kind", "");
        if ((!DelegatedSessionFile().empty() || kind == "user" ||
             kind == "attachment") &&
            (persist_ || !session_file_.empty())) {
          SaveSession(true);
        }
      });
}

Application::~Application() {
  context_.observability.Unsubscribe(message_subscription_);
}

AppSession Application::Session() {
  return AppSession{context_, attachments_, session_file_, saved_revision_};
}

int Application::Run() {
  int attachment_status = LoadInitialAttachments();
  if (attachment_status != 0) return attachment_status;
  if (!context_.options.prompt.empty() &&
      (context_.options.resume_latest || !session_file_.empty())) {
    if (std::string error = ResumeAtStartup(); !error.empty()) {
      // No teardown: saving now would overwrite the conversation with this
      // empty one. A headless run's stdout is its answer or its envelope.
      error = "cannot resume: " + error;
      context_.output.Restore();
      if (context_.options.json) {
        printf("%s\n", JsonDump(HeadlessResult("", error, json::array(), {},
                                               json::object(), 2))
                           .c_str());
      } else {
        fprintf(stderr, "%s\n", error.c_str());
        Emit(Event{EventId::kError, {{"error", error}}});
      }
      return 2;
    }
  }
  if (!context_.options.prompt.empty()) return RunHeadless();
  return channel_ ? RunChannel() : 2;
}

int Application::LoadInitialAttachments() {
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

void Application::ReloadConfigAtTurnBoundary() {
  std::optional<ConfigReload> reload =
      context_.config_manager.Reload(runtime_.config);
  // A document edited since may hold what cannot be taken: said when it
  // appears, not at every turn.
  if (const std::string& problem = context_.config_manager.Problem();
      problem != config_problem_) {
    config_problem_ = problem;
    if (!problem.empty()) {
      Emit(NoticeEvent(PresentationStatus::kWarned, problem));
    }
  }
  if (!reload) return;
  runtime_.config = reload->active;
  api_.config = reload->active;
  Emit(Event{EventId::kConfigChanged,
             {{"changed", reload->applied},
              {"deferred", reload->deferred},
              {"source", "config_file"}}});
  if (context_.options.prompt.empty() &&
      (!reload->applied.empty() || !reload->deferred.empty())) {
    std::string notice = "configuration reloaded for the next turn";
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

void Application::RunTurns(const std::string& input, json content,
                           const json& images) {
  EnsureSessionPath();
  ReloadConfigAtTurnBoundary();
  // Search an endpoint offers but no model is named for is withheld from the
  // model, so the person hears why once, when the route stops running it;
  // not when they turned the tool off themselves.
  const Tool* search = FindTool(context_.tools, "web_search");
  const std::vector<std::string> enabled = agent_.EnabledTools();
  const bool unnamed =
      search && search->available &&
      std::ranges::find(enabled, search->name) != enabled.end() &&
      api_.config.web_search_model.empty() && !search->available();
  if (unnamed && !search_unnamed_) {
    Emit(NoticeEvent(PresentationStatus::kWarned, kWebSearchNoModel));
  }
  search_unnamed_ = unnamed;
  PermissionControl(context_, json::object());
  // Publish lifecycle boundaries even when no model response is produced.
  if (channel_ || !session_file_.empty()) PublishChannelState(false);
  agent_.Turn(input, std::move(content), images, request_id_, origin_);
  SteeringState().Take();
#ifdef UAGENT_BROWSER
  // The turn is over: the shared browser is free for the next conversation.
  if (!context_.browser_lease.empty()) {
    browser::Request(
        {{"op", "release"}, {"session_id", context_.browser_lease}},
        browser::kProbeMs);
  }
#endif
  if (channel_ || !session_file_.empty()) PublishChannelState(false);
}

void Application::LogSessionEnd(const char* reason) const {
  Emit(Event{EventId::kSessionEnded,
             {{"reason", reason},
              {"usage", UsageJson(agent_.SessionUsage())},
              {"context_tokens", agent_.ContextUsed()}}});
}

void Application::Teardown(const char* reason) {
  runtime_.Shutdown();
  agent_.DrainActivities();
  agent_.AccountSideUsage();
  SaveSession(true);
  LogSessionEnd(reason);
  std::remove(UsageLedger().c_str());
  if (!session_file_.empty()) {
    std::string error;
    if (!context_.observability.Journal().Flush(
            session_file_ + kSessionJournalSuffix, error)) {
      fprintf(stderr, "cannot save session journal: %s\n", error.c_str());
    }
  }
  context_.observability.Flush();
}

std::string Application::ResumeAtStartup() {
  std::string previous_path = session_file_;
  std::string source = session_file_;
  if (source.empty() && context_.options.resume_latest) {
    std::vector<SessionInfo> sessions = ListSessions();
    if (!sessions.empty()) {
      source = sessions.front().path;
    } else if (!channel_) {
      // On stderr: a headless run's stdout is its answer.
      fputs("uagent: no saved conversation to continue; starting a new one\n",
            stderr);
    }
  }
  if (!source.empty() && PathExists(source)) {
    // A thread's sandbox, budget and approval default are applied by its
    // runtime; continued without one it would have none, and lose its role on
    // saving.
    if (!channel_ && JsonValue(SessionHeader(source), kSessionHeaderKind, "") ==
                         kSessionKindThread) {
      return "that session is a panel's thread; continue it in its "
             "conversation, or name another session";
    }
    std::string error;
    if (!ResumeInto(agent_, source, session_file_, error)) return error;
  }
  AppSession session = Session();
  LoadSessionJournal(session, previous_path);
  saved_revision_ = agent_.Revision();
  return {};
}

void Application::SaveSession(bool force) {
  if (!persist_ && session_file_.empty()) return;
  EnsureSessionPath();
  std::string error;
  if (!Session().Save(error, force)) {
    input_error_ = "cannot save session: " + error;
    fprintf(stderr, "%s\n", input_error_.c_str());
    Emit(Event{EventId::kError, {{"error", input_error_}}});
  }
}

void Application::EnsureSessionPath() {
  if (persist_ && session_file_.empty()) {
    session_file_ = HistoryPath(
        CanonicalCwd(), UtcStamp("%Y%m%dT%H%M%SZ") + "-" + MakeSessionId());
  }
  // Peer sessions address this process by its session file; exporting it
  // here covers the constructor path, resume, and first save alike.
  if (!session_file_.empty()) {
    ::setenv("UAGENT_INTERNAL_SESSION_PATH", session_file_.c_str(), 1);
  }
  runtime_.processes.SetOwner(
      session_file_.empty() ? agent_.SessionId() : SessionIdFor(session_file_));
}

void Application::ReportReplacedExecutable() {
  if (!ExecutableReplaced()) return;
  context_.observability.Emit(
      NoticeEvent(PresentationStatus::kNeutral,
                  "uagent was replaced on disk; restart to run the new build"));
}

void Application::RunPrompt(const std::string& input) {
  ReportReplacedExecutable();
  input_error_.clear();
  if (api_.model.empty()) {
    input_error_ = "no model set: choose one with /model";
    Emit(NoticeEvent(PresentationStatus::kFailed, input_error_));
    return;
  }
  json content;
  json images = json::array();
  if (!attachments_.empty()) {
    std::string error;
    content = AttachmentContent(input, attachments_, error);
    if (!error.empty()) {
      input_error_ = error;
      Emit(Event{EventId::kError, {{"error", error}}});
      return;
    }
    for (const auto& attachment : attachments_) {
      if (!attachment.asset_id.empty()) {
        images.push_back(AttachmentDisplayJson(attachment));
      }
    }
    attachments_.clear();
  }
  RunTurns(input, std::move(content), images);
}

json Application::InterfaceState() const {
  return {{"route", RouteSelection(api_, context_.provider.providers)},
          {"effort", api_.reasoning_effort},
          {"context_tokens", agent_.ContextUsed()},
          {"context_window", api_.ctx_window},
          {"attachments", attachments_.size()},
          {"background", runtime_.processes.Count()},
          {"tools", context_.tools.size()},
          {"yolo", ApprovalIsYolo()}};
}

void Application::ProcessInput(std::string input) {
  input = Trim(input);
  if (input.empty()) {
    if (!attachments_.empty()) RunPrompt(input);
    return;
  }
  ParsedSlashCommand command = ParseSlashCommand(input);
  if (command.spec) DebugLog("command", {{"command", command.spec->name}});
  if (std::string prompt = SlashCommandPrompt(command); !prompt.empty()) {
    RunPrompt(prompt);
    return;
  }
  if (command.spec) {
    AppSession session = Session();
    CommandReply reply = RunSlashCommand(session, command);
    Emit(Event{EventId::kCommandCompleted,
               {{"request_id", request_id_},
                {"command", command.spec->name},
                {"argument", command.argument},
                {"inspect", !command.spec->Has(kNoViewer)},
                {"output", Trim(reply.output)},
                {"result", std::move(reply.result)},
                {"state", InterfaceState()}}});
    return;
  }
  if (input[0] == '/') {
    const std::string nearest = NearestSlashCommand(input);
    Emit(NoticeEvent(
        PresentationStatus::kFailed,
        "unknown command " + input + "; " +
            (nearest.empty() ? "use /help" : "did you mean " + nearest + "?")));
    return;
  }
  RunPrompt(input);
}

int RunApplication(AppContext& context) { return Application(context).Run(); }

}  // namespace uagent
