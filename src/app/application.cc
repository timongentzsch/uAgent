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
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/bootstrap.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/mcp/rpc.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
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
        saved_revision_(agent_.Revision()) {}

  int Run() {
    int attachment_status = LoadInitialAttachments();
    if (attachment_status != 0) return attachment_status;
    if (!context_.options.prompt.empty() && context_.options.resume_latest) {
      ResumeAtStartup();
    }
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
      printf("%s· configuration reloaded for the next turn", DIM());
      if (!reload->deferred.empty()) {
        if (reload->deferred.size() == 1) {
          printf(" · 1 setting requires restart");
        } else {
          printf(" · %zu settings require restart", reload->deferred.size());
        }
      }
      printf("%s\n", RST());
    }
  }

  void RunTurns(std::string input, json content = nullptr) {
    ReloadConfigAtTurnBoundary();
    agent_.Turn(input, std::move(content));
    SteeringState().Take();
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
      json envelope = HeadlessResult(
          std::move(answer), std::move(error), agent_.LatestToolTrace(),
          agent_.SessionUsage(), agent_.RouteUsageJson(), exit_code);
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
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(context_.options.prompt, attachments_, error,
                                  api_.capabilities.image_input,
                                  !api_.config.image_model.empty());
      if (!error.empty()) {
        context_.output.Restore();
        return FinishHeadless("", std::move(error), 2);
      }
    }
    RunTurns(context_.options.prompt, std::move(content));
    // Background work is observational and never starts a model turn. Keep the
    // process alive long enough to publish completion and drain retained state.
    while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
      uint64_t generation = runtime_.processes.Generation();
      if (!agent_.DrainBackground()) {
        runtime_.processes.WaitForChange(generation);
      }
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

  void LoadSessionJournal(const std::string& previous_path) {
    if (session_file_.empty() || session_file_ == previous_path) return;
    std::string error;
    if (!context_.observability.Journal().Load(session_file_ + ".events.jsonl",
                                               error)) {
      fprintf(stderr, "cannot load session journal: %s\n", error.c_str());
    }
    Emit(Event{
        EventId::kSessionResumed,
        {{"model", api_.RequestModel()}, {"messages", agent_.MessageCount()}}});
  }

  void ResumeAtStartup() {
    std::string previous_path = session_file_;
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
    LoadSessionJournal(previous_path);
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
    if (!context_.observability.Journal().Flush(session_file_ + ".events.jsonl",
                                                error)) {
      fprintf(stderr, "cannot save session journal: %s\n", error.c_str());
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
        context_.observability.Journal().Clear();
        attachments_.clear();
        session_file_.clear();
        saved_revision_ = agent_.Revision();
        printf("%s· fresh session%s\n", DIM(), RST());
        break;
      case SlashCommandId::kSessions: {
        std::string chosen = PickSession();
        if (!chosen.empty()) {
          std::string previous_path = session_file_;
          ResumeInto(agent_, chosen, session_file_);
          LoadSessionJournal(previous_path);
          attachments_.clear();
          saved_revision_ = agent_.Revision();
        }
        break;
      }
      case SlashCommandId::kTrace:
        agent_.PrintTrace();
        break;
      case SlashCommandId::kVariant:
        HandleVariant(command.argument);
        break;
      case SlashCommandId::kVerbose:
        agent_.SetVerbose(!agent_.Verbose());
        printf("%s· verbose %s%s\n", DIM(),
               agent_.Verbose()
                   ? "ON — full reasoning and expanded bounded tool output"
                   : "off — compact reasoning and compact tool output",
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
        printf("%s· yolo %s%s\n", DIM(),
               context_.options.yolo ? "ON — auto-approving everything" : "off",
               RST());
        break;
      case SlashCommandId::kCompact:
        HandleCompact();
        break;
      case SlashCommandId::kContext: {
        json effective =
            context_.config_manager.DiagnosticJson(runtime_.config);
        effective["capabilities"] = api_.capabilities.DiagnosticJson();
        const json& sources = effective["sources"];
        auto source = [&](const char* key, std::string fallback = "runtime") {
          return sources.is_object() ? JsonValue(sources, key, fallback)
                                     : fallback;
        };
        std::string model_source =
            source("UAGENT_MODEL", source("OPENROUTER_MODEL"));
        std::string credential_source =
            source("UAGENT_API_KEY", source("OPENROUTER_API_KEY"));
        effective["route"] = {
            {"base_url", RedactedUrl(api_.base_url)},
            {"base_url_source", source("UAGENT_BASE_URL")},
            {"model", api_.RequestModel()},
            {"model_source", std::move(model_source)},
            {"credentials", api_.api_key.empty() || api_.api_key == "sk-noop"
                                ? "<unset>"
                                : "<set>"},
            {"credential_source", std::move(credential_source)},
            {"context_window", api_.ctx_window}};
        printf("%seffective configuration%s\n%s\n", BOLD(), RST(),
               TerminalSafe(JsonDump(effective, 2)).c_str());
        printf("%smodel request%s\n", BOLD(), RST());
        agent_.PrintContext();
        break;
      }
      case SlashCommandId::kCost:
        HandleCost();
        break;
      case SlashCommandId::kMemory:
        HandleMemory();
        break;
      case SlashCommandId::kTools:
        HandleTools();
        break;
      case SlashCommandId::kAttach:
        HandleAttach(command.argument);
        break;
      case SlashCommandId::kProcesses:
        HandleProcesses();
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
    TerminalSpinner spinner(true, SpinnerLabel("searching model catalogs"));
    ModelSearch search = SearchModels(api_, context_.provider.routes,
                                      context_.provider.providers, argument);
    spinner.Stop();
    if (AbortRequested()) {
      ClearAbort();
      printf("%s· model search cancelled%s\n", YEL(), RST());
      return;
    }
    std::optional<ModelCandidate> selected = PickModel(search, api_);
    if (!selected) return;
    ApplyRoute(api_, selected->route);
    SaveSelectedModel(selected->selection);
  }

  // The route in schema form; the host only earns a segment when no provider
  // scope was resolvable, since `openrouter/...` already names the provider.
  StatusView SessionStatusView() const {
    StatusView view{.context_used = agent_.ContextUsed(),
                    .model = RouteSelection(api_, context_.provider.providers),
                    .verbose = agent_.Verbose(),
                    .yolo = context_.options.yolo,
                    .attachments = attachments_.size(),
                    .background = runtime_.processes.Count()};
    if (view.model.find('/') == std::string::npos) {
      view.host = UrlHost(api_.base_url);
    }
    return view;
  }

  void HandleCost() const {
    json routes = agent_.RouteUsageJson();
    if (routes.empty()) {
      printf("%s· no session spend yet%s\n", DIM(), RST());
      return;
    }
    for (const auto& [route, usage] : routes.items()) {
      std::string cost = JsonValue(usage, "cost_reported", false)
                             ? FmtCost(JsonValue(usage, "cost", 0.0))
                             : "cost unavailable";
      // Tokens beside the cost say *why* a route is expensive — a large
      // evidence block reads very differently from many small turns.
      Usage spent;
      spent.input = JsonValue(usage, "input", int64_t{0});
      spent.output = JsonValue(usage, "output", int64_t{0});
      spent.cache_read = JsonValue(usage, "cache_read", int64_t{0});
      std::string tokens = TokenSummary(spent);
      std::string cache = CacheSummary(spent);
      if (!cache.empty()) tokens += " · " + cache;
      printf("%s· %s · %s · %s%s\n", DIM(), TerminalSafe(route).c_str(),
             tokens.c_str(), cost.c_str(), RST());
    }
    const Usage& session = agent_.SessionUsage();
    std::string totals = TokenSummary(session);
    std::string session_cache = CacheSummary(session);
    if (!session_cache.empty()) totals += " · " + session_cache;
    printf("%s· total · %s · %s", DIM(), totals.c_str(),
           session.cost_reported ? FmtCost(session.cost).c_str()
                                 : "cost unavailable");
    if (api_.config.session_budget > 0) {
      printf(" / %s", FmtCost(api_.config.session_budget).c_str());
    }
    printf("%s\n", RST());
  }

  // The startup row is a snapshot; MCP refresh and config reloads change the
  // set mid-session, so this is the live view.
  void HandleTools() const {
    const std::vector<Tool>& tools = context_.tools;
    printf("%s· %zu tools%s\n", DIM(), tools.size(), RST());
    for (const Tool& tool : tools) {
      printf("%s· %s%s\n", DIM(), TerminalSafe(tool.name).c_str(), RST());
    }
  }

  void HandleMemory() const {
    printf("%s· memory %s%s\n", DIM(),
           api_.config.memory_enabled ? "on" : "off", RST());
    if (!api_.config.memory_enabled) return;
    std::vector<MemoryEntry> entries = ListMemories();
    if (entries.empty()) {
      printf("%s· no saved memories%s\n", DIM(), RST());
      return;
    }
    std::map<std::string, MemoryEvent> latest;
    for (MemoryEvent& event : LoadMemoryEvents()) {
      if (!event.key.empty()) {
        latest[event.key + "\n" + event.workspace] = std::move(event);
      }
    }
    for (const MemoryEntry& entry : entries) {
      std::string workspace = entry.key.starts_with("project/")
                                  ? std::filesystem::path(entry.path)
                                        .parent_path()
                                        .filename()
                                        .string()
                                  : "";
      auto found = latest.find(entry.key + "\n" + workspace);
      if (found == latest.end()) {
        printf("%s· %s · %s%s\n", DIM(), TerminalSafe(entry.key).c_str(),
               TerminalSafe(Tilde(entry.path)).c_str(), RST());
        continue;
      }
      const MemoryEvent& event = found->second;
      printf("%s· %s · %s · %s", DIM(), TerminalSafe(entry.key).c_str(),
             TerminalSafe(event.action).c_str(),
             TerminalSafe(event.timestamp).c_str());
      if (event.automatic) {
        printf(" · automatic");
        if (!event.source_session.empty()) {
          printf(" · source %s", TerminalSafe(event.source_session).c_str());
        }
      } else {
        printf(" · explicit");
      }
      printf("%s\n", RST());
      if (!event.preview.empty()) {
        printf("%s  %s%s\n", DIM(), TerminalSafe(event.preview).c_str(), RST());
      }
      if (!event.previous.empty()) {
        printf("%s  replaced: %s%s\n", DIM(),
               TerminalSafe(event.previous).c_str(), RST());
      }
    }
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
    SaveSelectedModel(selected);
  }

  void SaveSelectedModel(const std::string& selected) {
    bool named_route = ResolveModelRoute(context_.provider.routes,
                                         context_.provider.providers, selected)
                           .has_value();
    ActivateCurrentRoute();
    std::string error;
    bool saved =
        SaveModelPreference({selected, api_.base_url, named_route}, error);
    DebugLog("route_changed", {{"route", selected},
                               {"model", api_.model},
                               {"base_url", api_.base_url},
                               {"effort", api_.reasoning_effort},
                               {"preference_saved", saved}});
    printf("%s· model %s%s\n", DIM(),
           RouteSelection(api_, context_.provider.providers).c_str(), RST());
    if (!saved) {
      printf("%s· model changed but preference was not saved: %s%s\n", YEL(),
             TerminalSafe(error).c_str(), RST());
    }
  }

  void ActivateCurrentRoute() {
    ActivateRoute(api_);
    agent_.RouteChanged();
  }

  void HandleEffort(const std::string& argument) {
    if (argument.empty()) {
      printf("%s· effort %s%s\n", DIM(),
             api_.reasoning_effort.empty() ? "default"
                                           : api_.reasoning_effort.c_str(),
             RST());
    } else if (argument == "default") {
      api_.reasoning_effort.clear();
      ActivateCurrentRoute();
      printf("%s· effort provider default%s\n", DIM(), RST());
    } else if (!ValidEffort(argument)) {
      printf(
          "%s· effort must be none, minimal, low, medium, high, xhigh, or "
          "max; use default to defer to the provider%s\n",
          RED(), RST());
    } else {
      api_.reasoning_effort = argument;
      ActivateCurrentRoute();
      printf("%s· effort %s%s\n", DIM(), argument.c_str(), RST());
    }
  }

  void HandleVariant(const std::string& argument) {
    if (!api_.capabilities.model_variants) {
      printf("%s· /variant is unavailable on the active route%s\n", RED(),
             RST());
      return;
    }
    std::string variant = argument;
    if (variant.starts_with(':')) variant.erase(0, 1);
    if (variant.empty()) {
      std::string label = api_.config.openrouter_variant.empty()
                              ? "default"
                              : ":" + api_.config.openrouter_variant;
      printf("%s· variant %s · choose default, nitro, floor, or exacto%s\n",
             DIM(), label.c_str(), RST());
      return;
    }
    if (variant == "default") variant.clear();
    if (!ValidOpenRouterVariant(variant)) {
      printf("%s· variant must be default, nitro, floor, or exacto%s\n", RED(),
             RST());
      return;
    }
    api_.config.openrouter_variant = variant;
    runtime_.config.openrouter_variant = variant;
    setenv("UAGENT_OPENROUTER_VARIANT", variant.c_str(), 1);
    ActivateCurrentRoute();
    const char* detail = "provider default";
    if (variant == "nitro") detail = "highest throughput";
    if (variant == "floor") detail = "lowest price";
    if (variant == "exacto") detail = "quality-first tool reliability";
    DebugLog("variant_changed",
             {{"variant", variant}, {"model", api_.RequestModel()}});
    std::string label = variant.empty() ? "default" : ":" + variant;
    printf("%s· variant %s — %s%s\n", DIM(), label.c_str(), detail, RST());
  }

  void HandleCompact() {
    agent_.Compact();
    SteeringState().Take();
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
    if (argument == "clear") {
      attachments_.clear();
      printf("%s· attachments cleared%s\n", DIM(), RST());
      return;
    }
    Attachment attachment;
    std::string error;
    if (!InspectAttachment(argument, attachment, error) ||
        !(error = ImageInputError(attachment, api_.capabilities.image_input,
                                  !api_.config.image_model.empty()))
             .empty()) {
      printf("%s%s%s\n", RED(), error.c_str(), RST());
      return;
    }
    attachments_.push_back(std::move(attachment));
    printf("%s· attached %s for the next message%s\n", DIM(),
           attachments_.back().name.c_str(), RST());
  }

  void HandleProcesses() const {
    ToolResult activities = ToolActivityList(runtime_.processes);
    if (activities.output.starts_with('(')) {
      printf("%s· %s%s\n", DIM(), activities.output.c_str(), RST());
      return;
    }
    printf("%sbackground work%s\n%s%s%s", BOLD(), RST(), DIM(),
           TerminalSafe(activities.output).c_str(), RST());
  }

  void RunPrompt(std::string input) {
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(input, attachments_, error,
                                  api_.capabilities.image_input,
                                  !api_.config.image_model.empty());
      if (!error.empty()) {
        printf("%s%s%s\n", RED(), error.c_str(), RST());
        return;
      }
      attachments_.clear();
    }
    RunTurns(std::move(input), std::move(content));
  }

  bool ProcessInput(std::string input) {
    input = Trim(input);
    if (input.empty()) return false;
    if (input[0] == '/') DebugLog("command", {{"command", input}});
    ParsedSlashCommand command = ParseSlashCommand(input);
    if (command.spec) return HandleCommand(command);
    if (input[0] == '/') {
      printf("%s· unknown command %s; use /help%s\n", RED(),
             TerminalSafe(input).c_str(), RST());
      return false;
    }
    RunPrompt(std::move(input));
    return false;
  }

  int RunPersistentInteractive() {
    InteractiveOutput output;
    if (!output.Start()) return -1;
    RawComposer composer(output);
    if (!composer.Start()) return -1;
    InputBroker broker;
    SetTerminalWakeFd(broker.NotifyFd());
    runtime_.processes.SetNotifyFd(broker.NotifyFd());
    SetInteractiveReadHandler([&broker](const std::string& prompt, bool* eof,
                                        bool keep_history,
                                        const std::string& initial) {
      return broker.Read(prompt, eof, keep_history, initial);
    });
    g_persistent_composer = true;

    std::thread worker;
    std::atomic<bool> working{false};
    std::atomic<bool> worker_quit{false};
    bool interrupting = false;
    bool exit_when_idle = false;
    bool answering = false;
    std::optional<std::string> next_input;
    std::string saved_draft;
    std::string pending_output;
    auto started = std::chrono::steady_clock::now();

    auto status = [&] {
      if (!working) {
        return StatusBar(api_, agent_.SessionUsage(), SessionStatusView());
      }
      auto now = std::chrono::steady_clock::now();
      double elapsed = std::chrono::duration<double>(now - started).count();
      size_t background = runtime_.processes.Count();
      char seconds[32];
      snprintf(seconds, sizeof seconds, "%.1fs", elapsed);
      std::string activity = CurrentTerminalActivity();
      static constexpr auto kSpinnerInterval = std::chrono::milliseconds(100);
      static constexpr const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                "⠴", "⠦", "⠧", "⠇", "⠏"};
      auto ticks =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - started) /
          kSpinnerInterval;
      std::string prefix = kFrames[static_cast<size_t>(ticks) % 10];
      prefix += " ";
      std::string state = interrupting
                              ? "interrupting"
                              : (activity.empty() ? "working" : activity);
      std::string suffix = " · " + std::string(seconds);
      suffix += " · ctx " + FmtCount(agent_.ContextSnapshot());
      if (background > 0) suffix += " · bg:" + std::to_string(background);
      size_t foreground = runtime_.processes.ForegroundCount();
      if (foreground > 0) {
        suffix += " · Ctrl+B background";
        if (foreground > 1) {
          suffix += " " + std::to_string(foreground) + " commands";
        }
      }
      size_t queued = SteeringState().QueuedCount();
      if (queued > 0) {
        suffix += " · steer:" + std::to_string(queued);
      }
      size_t width = TerminalWidth(1);
      if (SteeringEnabled()) {
        std::string hint = " · Esc interrupt";
        // A rolling ticker always holds more text than fits, so it asks for
        // the full cap instead of the width of its idle fallback label.
        size_t desired = std::min<size_t>(
            CurrentTerminalActivityRolling() ? 64 : DisplayWidth(state), 64);
        size_t with_hint = DisplayWidth(prefix) + DisplayWidth(suffix) +
                           DisplayWidth(hint) + desired;
        if (with_hint <= width) suffix += hint;
      }
      size_t reserved = DisplayWidth(prefix) + DisplayWidth(suffix);
      size_t activity_width = width > reserved ? width - reserved : 0;
      if (CurrentTerminalActivityRolling()) {
        // Rolling ticker: render a sliding window of the reasoning instead of
        // the static fallback label so it animates with the status frame.
        // ActivityLabel is a no-op while the window fits; on a terminal too
        // narrow even for the ticker label it bounds the row as usual.
        return prefix +
               ActivityLabel(RenderCurrentTerminalActivity(activity_width),
                             activity_width) +
               suffix;
      }
      return prefix + ActivityLabel(state, activity_width) + suffix;
    };

    constexpr auto kResizeSettle = std::chrono::milliseconds(80);
    std::optional<std::chrono::steady_clock::time_point> resize_settled;

    auto rendered_status = [&] { return StatusBarLine(status()); };

    auto status_state = [&] {
      return std::string(interrupting ? "interrupting|" : "working|") +
             CurrentTerminalActivity() + "|" +
             std::to_string(SteeringState().QueuedCount()) + "|" +
             std::to_string(runtime_.processes.Count()) + "|" +
             std::to_string(runtime_.processes.ForegroundCount());
    };

    auto unmount = [&] {
      if (!composer.Drawn()) return;
      output.Write("\r\033[" + std::to_string(composer.CaretRow() + 1) +
                   "A\033[J");
      composer.Detach();
    };

    // The one place the pinned region is painted: erase what is there, emit any
    // transcript text above it, then the status row and the composer. Every
    // caller differs only in the text it contributes and whether the composer
    // keeps its buffer, so geometry can only be wrong here.
    auto paint = [&](std::string text, const std::string* prompt,
                     const std::string& initial, bool keep_history) {
      unmount();
      if (!text.empty()) {
        if (text.back() != '\n') text += '\n';
        output.Write(text);
      }
      output.Write(rendered_status() + "\n");
      if (prompt) {
        composer.Mount(*prompt, initial, keep_history);
      } else {
        composer.Remount();
      }
    };

    auto mount = [&](const std::string& prompt = InputPrompt(),
                     const std::string& initial = std::string(),
                     bool keep_history = true) {
      paint({}, &prompt, initial, keep_history);
    };

    auto insert = [&](std::string text) {
      if (text.empty()) return;
      paint(std::move(text), nullptr, {}, true);
    };

    // A resize must *replace* the pinned region, not add to it: erasing only
    // downward leaves the status row that sits above the cursor, so every
    // repaint would append another one. Walk up to the status row first, the
    // way every other paint does.
    //
    // CaretRow() is exact when the block did not reflow — an empty or short
    // draft, or any widening, since the rows are separate lines. Narrowing
    // with a draft long enough to soft-wrap can leave one stale fragment
    // above; the next mount clears it. Pinning that down needs a cursor
    // position report, which is deliberately not in this change.
    auto resync = [&] {
      if (!composer.Drawn()) return;
      paint({}, nullptr, {}, true);
    };

    auto refresh_status = [&] {
      if (!composer.Drawn()) return;
      // Stay silent while a drag is still in flight. The width is read per
      // write, so a status row formatted for one width can land in a terminal
      // that has already become narrower — it wraps, the cursor is no longer
      // where the caller believes, and the next write starts mid-line. The
      // settle repaint restores the row once the size stops moving.
      if (resize_settled) return;
      // The status is always the one row immediately above the mounted
      // composer; update it without repainting the editor's footprint.
      size_t rows_up = composer.CaretRow() + 1;
      output.Write("\r\033[" + std::to_string(rows_up) + "A");
      output.Write(rendered_status());
      output.Write("\033[" + std::to_string(rows_up) + "B\r");
      if (composer.CaretColumn() > 0) {
        output.Write("\033[" + std::to_string(composer.CaretColumn()) + "C");
      }
    };

    auto flush_output = [&](bool all) {
      pending_output += output.Read();
      size_t split = all ? pending_output.size() : pending_output.rfind('\n');
      if (split == std::string::npos || split == 0) return;
      if (!all) ++split;
      std::string ready = pending_output.substr(0, split);
      pending_output.erase(0, split);
      insert(std::move(ready));
    };

    auto start_work = [&](std::string input) {
      agent_.ContextUsed();
      working = true;
      worker_quit = false;
      interrupting = false;
      started = std::chrono::steady_clock::now();
      worker = std::thread([&, input = std::move(input)]() mutable {
        worker_quit = ProcessInput(std::move(input));
        working = false;
        broker.Notify();
      });
    };

    mount();
    auto last_redraw = std::chrono::steady_clock::now();
    std::string last_state = status_state();
    std::vector<pollfd> events;
    events.reserve(3 + runtime_.mcp.Servers().size());
    while (!exit_when_idle || working) {
      events = {{STDIN_FILENO, POLLIN, 0},
                {output.Fd(), POLLIN, 0},
                {broker.Fd(), POLLIN, 0}};
      if (!working) {
        for (const auto& server : runtime_.mcp.Servers()) {
          if (server->alive && server->out >= 0) {
            events.push_back({server->out,
                              static_cast<int16_t>(POLLIN | POLLHUP | POLLERR),
                              0});
          }
        }
      }

      std::optional<std::chrono::steady_clock::time_point> wake_deadline =
          composer.WakeDeadline();
      if (working && !answering) {
        auto status_deadline = last_redraw + std::chrono::milliseconds(100);
        wake_deadline = wake_deadline
                            ? std::min(*wake_deadline, status_deadline)
                            : status_deadline;
      }
      if (resize_settled) {
        wake_deadline = wake_deadline
                            ? std::min(*wake_deadline, *resize_settled)
                            : *resize_settled;
      }
      int timeout_ms = -1;
      if (wake_deadline) {
        auto now = std::chrono::steady_clock::now();
        if (*wake_deadline <= now) {
          timeout_ms = 0;
        } else {
          timeout_ms = PollTimeoutMs(*wake_deadline);
        }
      }

      int ready = poll(events.data(), events.size(), timeout_ms);
      if (ready < 0 && errno != EINTR) break;
      if (g_terminal_resized) {
        g_terminal_resized = 0;
        // Dragging an edge emits a burst of SIGWINCH. The flag already
        // coalesces everything that arrives before this wake; the settle
        // window collapses the rest into one repaint instead of flickering
        // through every intermediate width.
        resize_settled = std::chrono::steady_clock::now() + kResizeSettle;
      }
      if (resize_settled &&
          std::chrono::steady_clock::now() >= *resize_settled) {
        resize_settled.reset();
        resync();
        last_redraw = std::chrono::steady_clock::now();
        last_state = status_state();
      }

      // Idle MCP stdout participates in the same poll set; any event also
      // drains messages buffered just before a worker released ownership.
      if (!working) McpDrainInbound(runtime_.mcp);

      if (events[1].revents & POLLIN) flush_output(false);
      if (events[2].revents & POLLIN) {
        broker.DrainWake();
        std::string prompt;
        std::string initial;
        bool keep_history = false;
        if (broker.Take(prompt, initial, keep_history)) {
          saved_draft = composer.Buffer();
          answering = true;
          mount(prompt, initial, keep_history);
        }
      }

      if ((events[0].revents & POLLIN) || composer.HasPending()) {
        InteractiveInputEvent event = composer.Read();
        if (event.kind != InteractiveInputKind::kNone) {
          if (event.kind == InteractiveInputKind::kLine && !answering) {
            // Submission has already printed the prompt below the status row.
            // Replace both regions so the transient working row does not enter
            // scrollback above steering or ordinary user input.
            size_t rows_up = composer.LastSubmittedRows() + 1;
            output.Write("\r\033[" + std::to_string(rows_up) + "A\033[J");
            output.Write("\r" + composer.Prompt() + TerminalSafe(event.text) +
                         "\n");
          }
          if (event.kind == InteractiveInputKind::kBackground) {
            if (!working || !runtime_.processes.RequestForegroundBackground()) {
              output.Write("\a");
            }
            refresh_status();
          } else if (answering) {
            bool eof = event.kind == InteractiveInputKind::kEscape ||
                       event.kind == InteractiveInputKind::kEof;
            broker.Answer(std::move(event.text), eof);
            answering = false;
            mount(InputPrompt(), saved_draft);
          } else if (event.kind == InteractiveInputKind::kEscape) {
            // A bare Escape clears the current input line (and any stashed
            // draft). Still honour a steering/interrupt request if the agent is
            // working, so Esc doubles as the interrupt key.
            saved_draft.clear();
            composer.Clear();
            if (working && SteeringEnabled() && !interrupting) {
              interrupting = true;
              SteeringState().Request();
              runtime_.processes.Wake();
              refresh_status();
            }
          } else if (event.kind == InteractiveInputKind::kEof) {
            exit_when_idle = true;
            mount();
          } else {
            std::string input = Trim(event.text);
            if (!input.empty()) {
              if ((input == "/q" || input == "/quit") && working) {
                exit_when_idle = true;
              } else if (working) {
                // Publish guidance before waking passive tool waits. Their
                // generation predicate makes this pairing lost-wakeup-safe.
                SteeringState().Queue(std::move(input));
                runtime_.processes.Wake();
              } else if (worker.joinable()) {
                next_input = std::move(input);
              } else {
                start_work(std::move(input));
              }
            }
            mount();
          }
        }
      }

      if (interrupting && !AbortRequested()) {
        interrupting = false;
        refresh_status();
      }

      if (!working && worker.joinable()) {
        worker.join();
        flush_output(true);
        bool activity_ready = agent_.DrainBackground();
        SaveSession();
        if (worker_quit) exit_when_idle = true;
        interrupting = false;
        if (!exit_when_idle && next_input) {
          start_work(std::move(*next_input));
          next_input.reset();
        }
        if (activity_ready) mount();
        refresh_status();
      }

      // ProcessSupervisor mirrors activity changes into the broker pipe, so
      // idle completion is handled without a periodic UI tick.
      if (!working && !worker.joinable() && !answering && !exit_when_idle) {
        if (agent_.DrainBackground()) {
          SaveSession();
          mount();
          refresh_status();
        }
      }

      if (working && !answering) {
        auto now = std::chrono::steady_clock::now();
        std::string current_state = status_state();
        bool state_changed = current_state != last_state;
        if (state_changed ||
            now - last_redraw >= std::chrono::milliseconds(100)) {
          refresh_status();
          last_redraw = now;
          last_state = std::move(current_state);
        }
      }
    }

    if (worker.joinable()) worker.join();
    flush_output(true);
    composer.Stop();
    SetInteractiveReadHandler({});
    runtime_.processes.SetNotifyFd(-1);
    SetTerminalWakeFd(-1);
    broker.Shutdown();
    g_persistent_composer = false;
    output.Stop();
    return 0;
  }

  int FinishInteractive(int status) {
    SaveSession();
    Teardown(exit_reason_.c_str());
    TerminalRestore();
    return status;
  }

  int RunInteractive() {
    ResumeAtStartup();
    persist_ = isatty(STDIN_FILENO);
    if (persist_ && AgentDepth() == 0 && api_.config.memory_enabled &&
        api_.config.memory_generate) {
      std::string extractor_error = StartMemoryExtractor(
          runtime_.processes, api_, CanonicalAccessPath(CanonicalCwd()),
          session_file_);
      if (!extractor_error.empty()) {
        DebugLog("memory_extract_start_error", {{"error", extractor_error}});
      }
    }
    if (persist_) {
      int persistent_status = RunPersistentInteractive();
      if (persistent_status >= 0) return FinishInteractive(persistent_status);
    }
    for (;;) {
      SaveSession();
      agent_.DrainBackground();
      PrintStatusBar(StatusBar(api_, agent_.SessionUsage(),
                               SessionStatusView()));
      bool eof = false;
      std::string line = ReadInputLine(InputPrompt(), &eof);
      if (eof) {
        if (g_tty) printf("\r\033[2K\r");
        printf("\n");
        break;
      }
      if (ProcessInput(std::move(line))) break;
    }
    return FinishInteractive(0);
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
