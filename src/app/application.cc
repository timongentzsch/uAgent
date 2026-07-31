// Copyright 2026 Timon Gentzsch

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/bootstrap.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/ui/display.h"
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
        saved_revision_(agent_.Revision()) {}

  int Run() {
    int attachment_status = LoadInitialAttachments();
    if (attachment_status != 0) return attachment_status;
    return context_.options.prompt.empty() ? RunInteractive() : RunHeadless();
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

  void RunTurns(std::string input, json content = nullptr) {
    bool resume = false;
    for (;;) {
      if (resume) {
        agent_.Resume();
      } else {
        agent_.Turn(input, std::move(content));
      }
      if (!g_steering.Take()) return;
      bool cancelled = false;
      std::string replacement = SteeringReplacement(cancelled);
      if (cancelled) {
        printf("%s· resuming%s\n", DIM(), RST());
        resume = true;
      } else {
        printf("%s· applying steering%s\n", DIM(), RST());
        input = std::move(replacement);
        resume = false;
      }
      content = nullptr;
    }
  }

  void LogSessionEnd(const char* reason) const {
    if (!g_debug.Enabled()) return;
    g_debug.Write("session_end", {{"reason", reason},
                                  {"usage", UsageJson(agent_.SessionUsage())},
                                  {"context_tokens", agent_.ContextUsed()}});
  }

  int RunHeadless() {
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(context_.options.prompt, attachments_, error);
      if (!error.empty()) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
      }
    }
    RunTurns(context_.options.prompt, std::move(content));
    context_.output.Restore();

    std::string ledger = EnvStr("UAGENT_USAGE_FILE");
    if (!ledger.empty()) {
      std::string error;
      if (!AppendPrivateLine(ledger, JsonDump(UsageJson(agent_.SessionUsage())),
                             error)) {
        fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
      }
    }
    std::string answer = agent_.LastText();
    runtime_.Shutdown();
    if (answer.empty()) {
      LogSessionEnd("headless_error");
      std::string error = agent_.LastError().empty()
                              ? "agent produced no answer"
                              : TerminalSafe(agent_.LastError());
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    LogSessionEnd("headless_complete");
    printf("%s\n", TerminalSafe(answer).c_str());
    return 0;
  }

  void ResumeAtStartup() {
    if (context_.options.resume_pick) {
      ResumeInto(agent_, PickSession(), session_file_);
    } else if (context_.options.resume_latest) {
      std::vector<SessionInfo> sessions = ListSessions();
      if (sessions.empty()) {
        printf("%s· no saved sessions%s\n", DIM(), RST());
      } else {
        ResumeInto(agent_, sessions.front().path, session_file_);
      }
    }
    saved_revision_ = agent_.Revision();
  }

  void SaveSession() {
    if (!persist_ || agent_.MessageCount() <= 1 ||
        agent_.Revision() == saved_revision_) {
      return;
    }
    if (session_file_.empty()) {
      session_file_ =
          UagentDir(kHistoryDir) + "/" + WorkspaceId(CanonicalCwd()) + "/" +
          UtcStamp("%Y%m%dT%H%M%SZ") + "-" + std::to_string(getpid()) + ".json";
    }
    std::error_code error_code;
    std::filesystem::path directory =
        std::filesystem::path(session_file_).parent_path();
    std::filesystem::create_directories(directory, error_code);
    chmod(directory.c_str(), 0700);
    std::string error;
    if (!agent_.Save(session_file_, error)) {
      fprintf(stderr, "cannot save session: %s\n", error.c_str());
      return;
    }
    saved_revision_ = agent_.Revision();
  }

  bool HandleCommand(const ParsedSlashCommand& command) {
    switch (command.spec->id) {
      case SlashCommandId::kQuit:
        exit_reason_ = "command";
        return true;
      case SlashCommandId::kReset:
        agent_.Reset();
        attachments_.clear();
        session_file_.clear();
        saved_revision_ = agent_.Revision();
        printf("%s· fresh session%s\n", DIM(), RST());
        break;
      case SlashCommandId::kSessions: {
        std::string chosen = PickSession();
        if (!chosen.empty()) {
          ResumeInto(agent_, chosen, session_file_);
          attachments_.clear();
          saved_revision_ = agent_.Revision();
        }
        break;
      }
      case SlashCommandId::kTrace:
        agent_.PrintTrace();
        break;
      case SlashCommandId::kVerbose:
        agent_.SetVerbose(!agent_.Verbose());
        printf("%s· verbose %s%s\n", DIM(),
               agent_.Verbose() ? "ON — expanded bounded tool output"
                                : "off — compact tool output",
               RST());
        break;
      case SlashCommandId::kHelp:
        PrintCommandHelp();
        break;
      case SlashCommandId::kModels:
        HandleModels(command.argument);
        break;
      case SlashCommandId::kModel:
        HandleModel(command.argument);
        break;
      case SlashCommandId::kEffort:
        HandleEffort(command.argument);
        break;
      case SlashCommandId::kYolo:
        context_.options.yolo = !context_.options.yolo;
        api_.server_tools_authorized = context_.options.yolo;
        printf("%s· yolo %s%s\n", DIM(),
               context_.options.yolo ? "ON — auto-approving everything" : "off",
               RST());
        break;
      case SlashCommandId::kCompact:
        HandleCompact();
        break;
      case SlashCommandId::kAttach:
        HandleAttach(command.argument);
        break;
      case SlashCommandId::kDetach:
        attachments_.clear();
        printf("%s· attachments cleared%s\n", DIM(), RST());
        break;
      case SlashCommandId::kOnline:
        HandleOnline();
        break;
    }
    return false;
  }

  void HandleModels(const std::string& argument) {
    if (argument.empty()) {
      printf(
          "%s· use /models QUERY to search every provider, or /models all "
          "for the full catalog%s\n",
          DIM(), RST());
      return;
    }
    std::string suffix =
        argument == "all" ? "" : " for " + TerminalSafe(argument);
    printf("%s· searching all model catalogs%s%s\n", DIM(), suffix.c_str(),
           RST());
    SteeringGuard steering;
    TerminalSpinner spinner(true, SpinnerLabel("searching model catalogs"));
    ModelSearch search = SearchModels(api_, context_.provider.routes,
                                      context_.provider.providers, argument);
    spinner.Stop();
    steering.Stop();
    if (AbortRequested()) {
      ClearAbort();
      printf("%s· model search cancelled%s\n", YEL(), RST());
      return;
    }
    std::optional<ModelCandidate> selected = PickModel(search, api_);
    if (!selected) return;
    ApplyRoute(api_, selected->route);
    bool named_route =
        ResolveModelRoute(context_.provider.routes, context_.provider.providers,
                          selected->selection)
            .has_value();
    SaveSelectedModel(selected->selection, named_route);
  }

  void HandleModel(const std::string& argument) {
    if (argument.empty()) {
      HandleModels("");
      return;
    }
    std::string selected = SelectModel(api_, context_.provider.routes,
                                       context_.provider.providers, argument);
    if (selected.empty()) {
      printf("%s· unknown model %s; use /models%s\n", RED(),
             TerminalSafe(argument).c_str(), RST());
      return;
    }
    bool named_route = ResolveModelRoute(context_.provider.routes,
                                         context_.provider.providers, selected)
                           .has_value();
    SaveSelectedModel(selected, named_route);
  }

  void SaveSelectedModel(const std::string& selected, bool named_route) {
    std::string error;
    bool saved =
        SaveModelPreference({selected, api_.base_url, named_route}, error);
    agent_.RouteChanged();
    DebugLog("route_changed", {{"route", selected},
                               {"model", api_.model},
                               {"base_url", api_.base_url},
                               {"effort", api_.reasoning_effort},
                               {"preference_saved", saved}});
    printf("%s· model %s · effort %s%s\n", DIM(), selected.c_str(),
           api_.reasoning_effort.empty() ? "default"
                                         : api_.reasoning_effort.c_str(),
           RST());
    if (!saved) {
      printf("%s· model changed but preference was not saved: %s%s\n", YEL(),
             TerminalSafe(error).c_str(), RST());
    }
  }

  void HandleEffort(const std::string& argument) {
    if (argument.empty()) {
      printf("%s· effort %s%s\n", DIM(),
             api_.reasoning_effort.empty() ? "default"
                                           : api_.reasoning_effort.c_str(),
             RST());
    } else if (argument == "default") {
      api_.reasoning_effort.clear();
      setenv("UAGENT_REASONING_EFFORT", "", 1);
      agent_.RouteChanged();
      printf("%s· effort provider default%s\n", DIM(), RST());
    } else if (!ValidEffort(argument)) {
      printf(
          "%s· effort must be none, minimal, low, medium, high, xhigh, or "
          "max; use default to defer to the provider%s\n",
          RED(), RST());
    } else {
      api_.reasoning_effort = argument;
      setenv("UAGENT_REASONING_EFFORT", argument.c_str(), 1);
      agent_.RouteChanged();
      printf("%s· effort %s%s\n", DIM(), argument.c_str(), RST());
    }
  }

  void HandleCompact() {
    for (;;) {
      agent_.Compact();
      if (!g_steering.Take()) return;
      bool cancelled = false;
      std::string next = SteeringReplacement(cancelled);
      if (cancelled) {
        printf("%s· resuming%s\n", DIM(), RST());
      } else {
        RunTurns(std::move(next));
        return;
      }
    }
  }

  void HandleAttach(const std::string& argument) {
    if (argument.empty()) {
      if (attachments_.empty()) {
        printf("%s· no pending attachments%s\n", DIM(), RST());
      } else {
        for (const Attachment& attachment : attachments_) {
          printf("%s· %s (%s)%s\n", DIM(),
                 TerminalSafe(attachment.path).c_str(), attachment.mime.c_str(),
                 RST());
        }
      }
      return;
    }
    Attachment attachment;
    std::string error;
    if (!InspectAttachment(argument, attachment, error) ||
        !(error = ImageInputError(attachment)).empty()) {
      printf("%s%s%s\n", RED(), error.c_str(), RST());
      return;
    }
    attachments_.push_back(std::move(attachment));
    printf("%s· attached %s for the next message%s\n", DIM(),
           attachments_.back().name.c_str(), RST());
  }

  void HandleOnline() {
    if (!OpenrouterUrl(api_.base_url)) {
      printf("%s· /online is available only for OpenRouter%s\n", RED(), RST());
      return;
    }
    bool enabled = api_.model.size() > 7 &&
                   api_.model.compare(api_.model.size() - 7, 7, ":online") == 0;
    if (enabled) {
      api_.model.erase(api_.model.size() - 7);
    } else {
      api_.model += ":online";
    }
    setenv("UAGENT_MODEL", api_.model.c_str(), 1);
    printf("%s· web search %s%s\n", DIM(),
           enabled ? "off"
                   : "ON — ~2K extra input tokens + search fees per request",
           RST());
  }

  void RunPrompt(std::string input) {
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(input, attachments_, error);
      if (!error.empty()) {
        printf("%s%s%s\n", RED(), error.c_str(), RST());
        return;
      }
      attachments_.clear();
    }
    RunTurns(std::move(input), std::move(content));
  }

  int RunInteractive() {
    ResumeAtStartup();
    persist_ = isatty(STDIN_FILENO);
    for (;;) {
      SaveSession();
      agent_.DrainBackground();
      PrintStatusBar(StatusBar(api_, agent_, context_.options.yolo,
                               attachments_.size(), runtime_.processes));
      bool eof = false;
      std::string line = ReadInputLine(InputPrompt(), &eof);
      if (eof) {
        if (g_tty) printf("\r\033[2K\r");
        printf("\n");
        break;
      }
      std::string input = Trim(line);
      if (input.empty()) continue;
      if (input[0] == '/') DebugLog("command", {{"command", input}});

      ParsedSlashCommand command = ParseSlashCommand(input);
      if (command.spec) {
        if (HandleCommand(command)) break;
        continue;
      }
      if (input[0] == '/') {
        printf("%s· unknown command %s; use /help%s\n", RED(),
               TerminalSafe(input).c_str(), RST());
        continue;
      }
      RunPrompt(std::move(input));
    }
    SaveSession();
    runtime_.Shutdown();
    std::remove(UsageLedger().c_str());
    LogSessionEnd(exit_reason_.c_str());
    TerminalRestore();
    return 0;
  }

  AppContext& context_;
  AppRuntime& runtime_;
  Api& api_;
  Agent& agent_;
  std::vector<Attachment> attachments_;
  std::string session_file_;
  uint64_t saved_revision_;
  bool persist_ = false;
  std::string exit_reason_ = "eof";
};

}  // namespace

int RunApplication(AppContext& context) { return Application(context).Run(); }

}  // namespace uagent
