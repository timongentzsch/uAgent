// Copyright 2026 Timon Gentzsch

#include "include/app/commands.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/self_description.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/child_agent.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/tools/process.h"
#include "include/tools/subagent.h"
#include "include/ui/sessions.h"

namespace uagent {

void SaveSessionSettings(AppSession& session) {
  session.ActiveAgent().SessionSettings(
      {{"route", RouteSelection(session.ApiClient(),
                                session.context.provider.providers)},
       {"permissions", session.context.permission_override.load()}});
}

void LoadSessionJournal(AppSession& session, const std::string& previous_path) {
  const json settings = session.ActiveAgent().SessionSettings();
  const std::string route = JsonValue(settings, "route", "");
  if (!route.empty() &&
      !session.context.options.overrides.contains("UAGENT_MODEL")) {
    if (SelectModel(session.ApiClient(), session.context.provider.routes,
                    session.context.provider.providers, route)
            .empty()) {
      Emit(NoticeEvent(PresentationStatus::kFailed,
                       "Saved model is unavailable: " + route));
    } else {
      ActivateRoute(session.ApiClient());
      session.ActiveAgent().RouteChanged();
    }
  }
  if (!session.context.options.yolo) {
    int mode = JsonValue(settings, "permissions", -1);
    session.context.permission_override.store(mode >= -1 && mode <= 1 ? mode
                                                                      : -1);
    PermissionControl(session.context, json::object());
    session.ActiveAgent().ApprovalChanged();
  }
  if (session.session_file.empty() || session.session_file == previous_path) {
    return;
  }
  session.context.session_approvals.clear();
  std::string error;
  if (!session.context.observability.Journal().Load(
          session.session_file + ".events.jsonl", error)) {
    Emit(NoticeEvent(PresentationStatus::kFailed,
                     "cannot load session journal: " + error));
  }
  Emit(Event{EventId::kSessionResumed,
             {{"model", session.ApiClient().RequestModel()},
              {"messages", session.ActiveAgent().MessageCount()}}});
}

StatusView SessionStatusView(const AppSession& session) {
  std::string model =
      RouteSelection(session.ApiClient(), session.context.provider.providers);
  std::string host = model.find('/') == std::string::npos
                         ? UrlHost(session.ApiClient().base_url)
                         : "";
  return StatusView{.context_used = session.ActiveAgent().ContextUsed(),
                    .model = std::move(model),
                    .host = std::move(host),
                    .verbose = session.ActiveAgent().Verbose(),
                    .yolo = ApprovalIsAutomatic(),
                    .attachments = session.attachments.size(),
                    .background = session.Runtime().processes.Count()};
}

namespace {

void ActivateCurrentRoute(AppSession& session) {
  ActivateRoute(session.ApiClient());
  session.ActiveAgent().RouteChanged();
}

void SaveSelectedModel(AppSession& session, const std::string& selected) {
  ModelSelection selection = ParseModelSelection(selected);
  bool named_route =
      ResolveModelRoute(session.context.provider.routes,
                        session.context.provider.providers, selection.base)
          .has_value();
  ActivateCurrentRoute(session);
  std::string error;
  bool saved = SaveModelPreference(
      {selected, session.ApiClient().base_url, named_route}, error);
  DebugLog("route_changed", {{"route", selected},
                             {"model", session.ApiClient().model},
                             {"base_url", session.ApiClient().base_url},
                             {"effort", session.ApiClient().reasoning_effort},
                             {"preference_saved", saved}});
  printf("%s· model %s%s\n", DIM(),
         RouteSelection(session.ApiClient(), session.context.provider.providers)
             .c_str(),
         RST());
  if (!saved) {
    printf("%s· model changed but preference was not saved: %s%s\n", YEL(),
           TerminalSafe(error).c_str(), RST());
  }
}

// The interactive catalog picker: prints the matches, then reads a choice.
// Its only caller is /models, so it stays here rather than in a header.
std::optional<ModelCandidate> PickModel(
    ModelSearch search, Api& api, const std::vector<NamedProvider>& providers) {
  std::string current = RouteSelection(api, {});
  json options = json::array();
  for (size_t i = 0; i < search.matches.size(); ++i) {
    const ModelCandidate& candidate = search.matches[i];
    bool active = candidate.route.base_url == api.base_url &&
                  candidate.route.model == api.model;
    if (active) {
      current = candidate.selection;  // already a selection the user can type
      if (candidate.info.context > 0) {
        api.ctx_window = candidate.info.context;
        setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
      }
    }
    printf("%s[%zu]%s %s%c %s", CYAN(), i + 1, RST(), active ? BOLD() : DIM(),
           active ? '*' : ' ', TerminalSafe(candidate.selection).c_str());
    std::string effort = active ? api.reasoning_effort : candidate.route.effort;
    if (effort.empty()) effort = candidate.info.default_effort;
    printf(" · effort %s", effort.empty() ? "default" : effort.c_str());
    if (candidate.info.context > 0) {
      printf(" · ctx %s", FmtCount(candidate.info.context).c_str());
    }
    if (!candidate.info.efforts.empty()) {
      printf(" · supports ");
      for (size_t index = 0; index < candidate.info.efforts.size(); ++index) {
        printf("%s%s", index ? "," : "", candidate.info.efforts[index].c_str());
      }
    }
    printf("%s\n", RST());
    std::string route_label =
        active ? RouteSelection(api, providers)
               : RouteSelection(SideRoute{.model = candidate.route.model,
                                          .base_url = candidate.route.base_url,
                                          .effort = effort},
                                providers);
    options.push_back({{"value", std::to_string(i + 1)},
                       {"route", std::move(route_label)},
                       {"label", candidate.selection},
                       {"active", active},
                       {"effort", effort},
                       {"context", candidate.info.context},
                       {"supported_efforts", candidate.info.efforts}});
  }
  for (const std::string& unavailable : search.unavailable) {
    printf("%s· %s catalog unavailable%s\n", YEL(),
           TerminalSafe(unavailable).c_str(), RST());
  }
  printf("%s· %zu model%s%s\n", DIM(), search.matches.size(),
         search.matches.size() == 1 ? "" : "s", RST());
  fflush(stdout);
  if (search.matches.empty()) return std::nullopt;

  bool cancelled = false;
  bool eof = false;
  std::string answer = ReadChoiceLine(
      {.kind = "model.select",
       .prompt = "model # (blank/Esc keeps " + TerminalSafe(current) + "): ",
       .options = std::move(options)},
      cancelled, eof);
  if (cancelled || eof || answer.empty()) {
    printf("%s· keeping %s%s\n", DIM(), TerminalSafe(current).c_str(), RST());
    return std::nullopt;
  }
  int64_t selected = 0;
  if (!ParseInt64(answer.c_str(), selected) || selected < 1 ||
      selected > static_cast<int64_t>(search.matches.size())) {
    printf("%s· not a listed number; keeping %s%s\n", YEL(),
           TerminalSafe(current).c_str(), RST());
    return std::nullopt;
  }
  return std::move(search.matches[static_cast<size_t>(selected - 1)]);
}

void HandleModels(AppSession& session, const std::string& argument) {
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
  fflush(stdout);
  TerminalSpinner spinner(session.context.channel == nullptr,
                          SpinnerLabel("searching model catalogs"));
  ModelSearch search =
      SearchModels(session.ApiClient(), session.context.provider.routes,
                   session.context.provider.providers, argument);
  spinner.Stop();
  if (AbortRequested()) {
    ClearAbort();
    printf("%s· model search cancelled%s\n", YEL(), RST());
    return;
  }
  if (session.context.channel && search.matches.empty()) {
    Emit(NoticeEvent(
        PresentationStatus::kFailed,
        search.unavailable.empty()
            ? "No matching models."
            : "Model catalogs are unavailable. Check provider settings."));
  }
  if (session.context.channel && search.matches.size() > 256) {
    search.matches.resize(256);
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "Showing the first 256 models; use /models QUERY to "
                     "narrow the catalog."));
  }
  std::optional<ModelCandidate> selected =
      PickModel(std::move(search), session.ApiClient(),
                session.context.provider.providers);
  if (!selected) return;
  ApplyRoute(session.ApiClient(), selected->route);
  SaveSelectedModel(session, selected->selection);
}

// The route in schema form; the host only earns a segment when no provider
// scope was resolvable, since `openrouter/...` already names the provider.

void HandleModel(AppSession& session, const std::string& argument) {
  if (argument.empty()) {
    HandleModels(session, "");
    return;
  }
  ModelSelection requested = ParseModelSelection(argument);
  std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, argument);
  if (selected.empty()) {
    printf("%s· unknown model %s; use /models%s\n", RED(),
           TerminalSafe(argument).c_str(), RST());
    return;
  }
  if (!requested.effort.empty() &&
      requested.effort != session.ApiClient().reasoning_effort) {
    printf("%s· effort %s is not supported by this model; using %s%s\n", YEL(),
           requested.effort.c_str(),
           session.ApiClient().reasoning_effort.empty()
               ? "provider default"
               : session.ApiClient().reasoning_effort.c_str(),
           RST());
  }
  SaveSelectedModel(session, selected);
}

// /effort and /variant change the same saved selection /model owns, so an
// interactive choice cannot silently evaporate on restart. A session with no
// saved preference yet stays session-only and says so.
void PersistSelectionSuffix(AppSession& session) {
  const Api& api = session.ApiClient();
  std::string error;
  if (SaveSelectionSuffix(api.capabilities.model_variants
                              ? api.config.openrouter_variant
                              : std::string(),
                          api.reasoning_effort, error)) {
    return;
  }
  printf("%s· this session only — %s%s\n", DIM(), TerminalSafe(error).c_str(),
         RST());
}

void HandleEffort(AppSession& session, const std::string& argument) {
  if (ValidEffort(argument)) {
    ProbeModel(session.ApiClient(), /*discover_efforts=*/true);
  }
  if (argument.empty()) {
    printf("%s· effort %s%s\n", DIM(),
           session.ApiClient().reasoning_effort.empty()
               ? "default"
               : session.ApiClient().reasoning_effort.c_str(),
           RST());
  } else if (argument == "default") {
    session.ApiClient().reasoning_effort.clear();
    ActivateCurrentRoute(session);
    printf("%s· effort provider default%s\n", DIM(), RST());
    PersistSelectionSuffix(session);
  } else if (!ValidEffort(argument)) {
    printf(
        "%s· effort must be none, minimal, low, medium, high, xhigh, or "
        "max; use default to defer to the provider%s\n",
        RED(), RST());
  } else if (!SupportsReasoningEffort(session.ApiClient(), argument)) {
    printf("%s· effort %s is not supported by the active model%s\n", RED(),
           argument.c_str(), RST());
  } else {
    session.ApiClient().reasoning_effort = argument;
    ActivateCurrentRoute(session);
    printf("%s· effort %s%s\n", DIM(), argument.c_str(), RST());
    PersistSelectionSuffix(session);
  }
}

void HandleVariant(AppSession& session, const std::string& argument) {
  if (!session.ApiClient().capabilities.model_variants) {
    printf("%s· /variant is unavailable on the active route%s\n", RED(), RST());
    return;
  }
  std::string variant = argument;
  if (variant.starts_with(':')) variant.erase(0, 1);
  if (variant.empty()) {
    std::string label =
        session.ApiClient().config.openrouter_variant.empty()
            ? "default"
            : ":" + session.ApiClient().config.openrouter_variant;
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
  session.ApiClient().config.openrouter_variant = variant;
  session.Runtime().config.openrouter_variant = variant;
  setenv("UAGENT_OPENROUTER_VARIANT", variant.c_str(), 1);
  ActivateCurrentRoute(session);
  const char* detail = "provider default";
  if (variant == "nitro") detail = "highest throughput";
  if (variant == "floor") detail = "lowest price";
  if (variant == "exacto") detail = "quality-first tool reliability";
  DebugLog("variant_changed", {{"variant", variant},
                               {"model", session.ApiClient().RequestModel()}});
  std::string label = variant.empty() ? "default" : ":" + variant;
  printf("%s· variant %s — %s%s\n", DIM(), label.c_str(), detail, RST());
  PersistSelectionSuffix(session);
}

void HandleCompact(AppSession& session) {
  session.ActiveAgent().Compact();
  SteeringState().Take();
}

void HandleAttach(AppSession& session, const std::string& argument) {
  if (argument.empty()) {
    if (session.attachments.empty()) {
      printf("%s· no pending attachments%s\n", DIM(), RST());
    } else {
      for (const Attachment& attachment : session.attachments) {
        printf("%s· %s (%s)%s\n", DIM(), TerminalSafe(attachment.path).c_str(),
               attachment.mime.c_str(), RST());
      }
    }
    return;
  }
  if (argument == "clear") {
    session.attachments.clear();
    printf("%s· attachments cleared%s\n", DIM(), RST());
    return;
  }
  Attachment attachment;
  std::string error;
  if (!InspectAttachment(argument, attachment, error) ||
      !(error = ImageInputError(
            attachment, session.ApiClient().capabilities.image_input,
            !session.ApiClient().config.image_model.empty()))
           .empty()) {
    printf("%s%s%s\n", RED(), error.c_str(), RST());
    return;
  }
  session.attachments.push_back(std::move(attachment));
  printf("%s· attached %s for the next message%s\n", DIM(),
         session.attachments.back().name.c_str(), RST());
}

void HandleCost(const AppSession& session) {
  json routes = session.ActiveAgent().RouteUsageJson();
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
  const Usage& spent = session.ActiveAgent().SessionUsage();
  std::string totals = TokenSummary(spent);
  std::string session_cache = CacheSummary(spent);
  if (!session_cache.empty()) totals += " · " + session_cache;
  printf(
      "%s· total · %s · %s", DIM(), totals.c_str(),
      spent.cost_reported ? FmtCost(spent.cost).c_str() : "cost unavailable");
  if (session.ApiClient().config.session_budget > 0) {
    printf(" / %s", FmtCost(session.ApiClient().config.session_budget).c_str());
  }
  printf("%s\n", RST());
}

// What the session actually resolved to: the effective configuration with the
// source of each setting, then the request the model would see.
void HandleContext(AppSession& session) {
  json effective =
      session.context.config_manager.DiagnosticJson(session.Runtime().config);
  effective["capabilities"] = session.ApiClient().capabilities.DiagnosticJson();
  // The writable roots are the whole of what the sandbox permits, so the deep
  // view is the one place they are printed in full.
  effective["sandbox"] = SandboxDiagnosticJson();
  const json& sources = effective["sources"];
  auto source = [&](const char* key, const std::string& fallback = "runtime") {
    return sources.is_object() ? JsonValue(sources, key, fallback) : fallback;
  };
  std::string model_source = source("UAGENT_MODEL", source("OPENROUTER_MODEL"));
  std::string credential_source =
      source("UAGENT_API_KEY", source("OPENROUTER_API_KEY"));
  effective["route"] = {
      {"base_url", RedactedUrl(session.ApiClient().base_url)},
      {"base_url_source", source("UAGENT_BASE_URL")},
      {"model", session.ApiClient().RequestModel()},
      {"model_source", std::move(model_source)},
      {"credentials", session.ApiClient().api_key.empty() ||
                              session.ApiClient().api_key == "sk-noop"
                          ? "<unset>"
                          : "<set>"},
      {"credential_source", std::move(credential_source)},
      {"context_window", session.ApiClient().ctx_window}};
  printf("%seffective configuration%s\n%s\n", BOLD(), RST(),
         TerminalSafe(JsonDump(effective, 2)).c_str());
  printf("%smodel request%s\n", BOLD(), RST());
  if (session.context.channel == nullptr) {
    session.ActiveAgent().PrintContext();
  }
}

// The startup row is a snapshot; MCP refresh and config reloads change the
// set mid-session, so this is the live view.

// /context is the deep live-context view. /status answers the everyday
// questions in one screen and /debug-config explains provenance.
void HandleStatus(const AppSession& session) {
  json status = DescribeSelf(
      SelfTopic::kStatus, "",
      SelfDescriptionInputs{session.context.config_manager,
                            session.Runtime().config, session.ApiClient(),
                            session.context.tools, ApprovalIsAutomatic()});
  auto row = [](const char* label, const std::string& value) {
    printf("  %s%-16s%s %s\n", DIM(), label, RST(),
           TerminalSafe(value).c_str());
  };
  printf("%s\u00b5Agent %s%s\n", BOLD(), kVersion, RST());
  row("route", JsonValue(status, "route", std::string()));
  row("wire api", JsonValue(status, "wire_api", std::string()));
  row("endpoint", JsonValue(status, "base_url", std::string()));
  row("effort", JsonValue(status, "effort", std::string()));
  row("approval", JsonValue(status, "approval", std::string()));
  row("web search", JsonValue(status, "web_search", std::string()));
  row("sandbox", JsonValue(status["sandbox"], "summary", std::string()));
  row("memory", JsonValue(status, "memory", false) ? "on" : "off");
  row("tools", std::to_string(JsonValue(status, "tools", int64_t{0})));
  int64_t window = JsonValue(status, "context_window", int64_t{0});
  row("context",
      window > 0 ? FmtCount(window) : std::string("provider default"));
  double budget = JsonValue(status, "session_budget", 0.0);
  if (budget > 0) row("session budget", FmtCost(budget));
  const json& restart = status["restart_required"];
  if (restart.is_array() && !restart.empty()) {
    std::string names;
    for (const json& key : restart) {
      names += (names.empty() ? "" : ", ") + key.get<std::string>();
    }
    row("restart needed", names);
  }
}

void HandleDebugConfig(const AppSession& session, const std::string& argument) {
  json described = DescribeSelf(
      SelfTopic::kConfig, argument,
      SelfDescriptionInputs{session.context.config_manager,
                            session.Runtime().config, session.ApiClient(),
                            session.context.tools, ApprovalIsAutomatic()});
  const json& settings = described["settings"];
  if (settings.empty()) {
    printf("%s\u00b7 no setting named %s%s\n", RED(),
           TerminalSafe(argument).c_str(), RST());
    return;
  }
  // Only settings the user actually influenced, unless one was named: the full
  // schema belongs in the generated reference, not in a terminal dump.
  bool named = !argument.empty();
  printf("%sconfiguration%s\n", BOLD(), RST());
  for (const json& setting : settings) {
    std::string source = JsonValue(setting, "source", std::string("default"));
    if (!named && source == "default") continue;
    std::string name = JsonValue(setting, "name", std::string());
    printf("  %s%s%s\n", BOLD(), TerminalSafe(name).c_str(), RST());
    printf("    %ssource%s      %s\n", DIM(), RST(),
           TerminalSafe(source).c_str());
    if (setting.contains("active")) {
      printf("    %sactive%s      %s\n", DIM(), RST(),
             TerminalSafe(JsonDump(setting["active"])).c_str());
    }
    printf("    %sdefault%s     %s\n", DIM(), RST(),
           TerminalSafe(JsonDump(setting["default"])).c_str());
    printf("    %stakes effect%s %s\n", DIM(), RST(),
           TerminalSafe(JsonValue(setting, "takes_effect", std::string()))
               .c_str());
  }
  const json& restart = described["restart_required"];
  if (restart.is_array() && !restart.empty()) {
    printf("%s\u00b7 restart required for %zu changed setting%s%s\n", YEL(),
           restart.size(), restart.size() == 1 ? "" : "s", RST());
  }
  printf(
      "%s\u00b7 precedence: command line, process environment, trusted "
      "project config, user config, built-in default%s\n",
      DIM(), RST());
}

void HandleTools(const AppSession& session) {
  const std::vector<Tool>& tools = session.context.tools;
  printf("%s· %zu tools%s\n", DIM(), tools.size(), RST());
  for (const Tool& tool : tools) {
    printf("%s· %s%s\n", DIM(), TerminalSafe(tool.name).c_str(), RST());
  }
}

// The collaborator records the subagent tool reports, joined with what the
// supervisor knows about the ones still running. The id is the join key: it is
// what the spawn stamped on the job, and it is what the human types back.
json AgentsJson(const AppSession& session) {
  const ProcessSupervisor& processes = session.Runtime().processes;
  std::vector<SubagentView> live = processes.SubagentViews();
  json rows = json::array();
  for (json& record : CollaboratorSummaries(processes)) {
    const std::string id = JsonValue(record, "id", std::string());
    for (const SubagentView& view : live) {
      if (view.source_id != id) continue;
      record["elapsed_ms"] =
          std::chrono::duration_cast<std::chrono::milliseconds>(view.elapsed)
              .count();
      if (!view.tail.empty()) record["progress"] = view.tail;
      break;
    }
    rows.push_back(std::move(record));
  }
  return rows;
}

SelfDescriptionInputs DescriptionInputs(const AppSession& session) {
  return {session.context.config_manager, session.Runtime().config,
          session.ApiClient(), session.context.tools, ApprovalIsAutomatic()};
}

json CommandResult(const AppSession& session,
                   const ParsedSlashCommand& command) {
  switch (command.spec->id) {
    case SlashCommandId::kHelp:
      return DescribeSelf(SelfTopic::kCommands, "", DescriptionInputs(session));
    case SlashCommandId::kStatus:
      return DescribeSelf(SelfTopic::kStatus, "", DescriptionInputs(session));
    case SlashCommandId::kDebugConfig:
      return DescribeSelf(SelfTopic::kConfig, command.argument,
                          DescriptionInputs(session));
    case SlashCommandId::kTools:
      return DescribeSelf(SelfTopic::kTools, "", DescriptionInputs(session));
    case SlashCommandId::kTrace:
      return {{"trace", session.ActiveAgent().LatestToolTrace()}};
    case SlashCommandId::kContext:
      return {
          {"effective_config", session.context.config_manager.DiagnosticJson(
                                   session.Runtime().config)},
          {"capabilities", session.ApiClient().capabilities.DiagnosticJson()},
          {"model_request", session.ActiveAgent().ModelRequest()}};
    case SlashCommandId::kCost:
      return {{"routes", session.ActiveAgent().RouteUsageJson()},
              {"total", UsageJson(session.ActiveAgent().SessionUsage())},
              {"session_budget", session.ApiClient().config.session_budget}};
    case SlashCommandId::kMemory:
    case SlashCommandId::kSkills:
    case SlashCommandId::kSchedule:
      return json::object();  // management commands return their operation
                              // result directly
    case SlashCommandId::kProcesses: {
      ToolResult activities = ToolActivityList(session.Runtime().processes);
      return {{"activities", activities.output}};
    }
    case SlashCommandId::kAgents:
      return {{"collaborators", AgentsJson(session)}};
    case SlashCommandId::kAttach: {
      json attachments = json::array();
      for (const Attachment& attachment : session.attachments) {
        attachments.push_back({{"name", attachment.name},
                               {"path", attachment.path},
                               {"mime", attachment.mime}});
      }
      return {{"attachments", std::move(attachments)}};
    }
    default:
      return json::object();
  }
}

}  // namespace

json PermissionControl(AppContext& context, const json& request) {
  std::string mode = JsonValue(request, "mode", "");
  if (!mode.empty()) {
    if (mode != "default" && mode != "ask" && mode != "yolo") {
      return {{"error", "unknown permission mode"}};
    }
    context.permission_override.store(mode == "default" ? -1
                                      : mode == "yolo"  ? 1
                                                        : 0);
  }
  auto configured = context.config_manager.Read();
  auto value = configured.values.find("UAGENT_APPROVAL");
  bool automatic = value != configured.values.end() && value->second == "yolo";
  const std::string default_mode = automatic ? "yolo" : "ask";
  int override = context.permission_override.load();
  if (override >= 0) automatic = override == 1;
  SetApprovalAutomatic(automatic);
  json result = {{"mode", override < 0 ? "default"
                          : override   ? "yolo"
                                       : "ask"},
                 {"effective", automatic ? "yolo" : "ask"},
                 {"default", default_mode}};
  if (!mode.empty()) {
    Emit(Event{EventId::kConfigChanged, {{"permissions", result}}});
  }
  return result;
}

json SessionControl(AppSession& session, const json& request) {
  std::string kind = JsonValue(request, "kind", "");
  if (kind == "permissions") return PermissionControl(session.context, request);
  if (kind == "fork") {
    std::string error;
    if (session.session_file.empty()) {
      session.session_file = UagentDir(kHistoryDir) + "/" +
                             WorkspaceId(CanonicalCwd()) + "/" +
                             MakeSessionId() + ".json";
    }
    SaveSessionSettings(session);
    if (!session.ActiveAgent().Save(session.session_file, error)) {
      return {{"error", error}};
    }
    return SessionStore::Fork(session.session_file,
                              JsonValue(request, "title", ""), true);
  }
  if (kind == "config") {
    return ConfigurationControl(
        request, session.context.config_manager, session.Runtime().config,
        session.context.config_manager.ProjectTrusted());
  }
  if (kind == "context") {
    auto exchanges = session.ActiveAgent().HttpExchanges();
    return {{"exchanges",
             exchanges.empty()
                 ? json::array({session.ActiveAgent().PreviewContext()})
                 : exchanges}};
  }

  if (JsonValue(request, "kind", "") == "activity") {
    if (JsonValue(request, "operation", "") != "followup") {
      return ActivityControl(session.Runtime().processes, request);
    }
    Tool tool =
        SubagentTool(session.ApiClient(), session.Runtime().processes,
                     session.context.provider.routes,
                     session.context.provider.providers, Debug().Enabled());
    ToolResult result =
        tool.run({{"operation", "followup"},
                  {"agent_id", JsonValue(request, "agent_id", "")},
                  {"prompt", JsonValue(request, "text", "")},
                  {"background", true}},
                 ToolContext{});
    return result.Ok() ? json{{"output", result.output}}
                       : json{{"error", result.output}};
  }
  if (JsonValue(request, "operation", "") == "catalog") {
    return ModelCatalogue(session.ApiClient(), session.context.provider.routes,
                          session.context.provider.providers,
                          JsonValue(request, "query", "all"));
  }
  if (JsonValue(request, "operation", "") != "select") {
    return {{"error", "unknown model operation"}};
  }
  std::string value = JsonValue(request, "model", "");
  ModelSelection selection = ParseModelSelection(value);
  std::string variant = JsonValue(request, "variant", "default");
  std::string effort = JsonValue(request, "effort", "default");
  if (variant == "default") variant.clear();
  if (effort == "default") effort.clear();
  if (selection.base.empty() || !ValidOpenRouterVariant(variant) ||
      (!effort.empty() && !ValidEffort(effort))) {
    return {{"error", "invalid model selection"}};
  }
  value = selection.base;
  if (!variant.empty()) value += ":" + variant;
  if (!effort.empty()) value += ":" + effort;
  std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, value);
  if (selected.empty()) return {{"error", "unknown model"}};
  SaveSelectedModel(session, selected);
  return {{"route", RouteSelection(session.ApiClient(),
                                   session.context.provider.providers)}};
}

json ActivityControl(ProcessSupervisor& processes, const json& request) {
  std::string operation = JsonValue(request, "operation", "list");
  if (operation == "list") return {{"activities", processes.ActivityViews()}};
  int64_t id = JsonValue(request, "activity_id", int64_t{0});
  auto job = processes.Find(id);
  std::string agent = job ? job->source_id : JsonValue(request, "agent_id", "");
  if (operation == "inspect") {
    json result = job ? processes.InspectActivity(id) : json::object();
    if (!agent.empty()) {
      result.update(InspectCollaborator(
          processes, agent, JsonValue(request, "before", uint64_t{0})));
    }
    return result.empty()
               ? json{{"error", "activity unavailable in this conversation"}}
               : result;
  }
  if (!job) return {{"error", "activity unavailable in this conversation"}};
  ToolResult result;
  if (operation == "stop") {
    result = ToolActivityStop(processes, id);
  } else if (operation == "message" && !job->source_id.empty() &&
             !JsonValue(request, "text", "").empty()) {
    result =
        WriteCollaboratorMail(job->source_id, JsonValue(request, "text", ""));
  } else {
    return {{"error", "unsupported activity operation"}};
  }
  return result.Ok() ? json{{"output", result.output}, {"operation", operation}}
                     : json{{"error", result.output}};
}

std::string ActivityText(const json& result) {
  for (const char* key : {"activities", "collaborators"}) {
    if (const json* rows = JsonArray(result, key)) {
      std::string text =
          std::string(std::string_view(key) == "activities" ? "background work"
                                                            : key) +
          " (" + FmtCount(static_cast<int64_t>(rows->size())) + ")\n";
      for (const json& row : *rows) {
        if (JsonValue(row, "detached", false)) text += "[detached] activity ";
        auto id = row.find("id");
        text += id == row.end()   ? "—"
                : id->is_string() ? id->get<std::string>()
                                  : JsonDump(*id);
        text += "  " + JsonValue(row, "status", "") + " · " +
                JsonValue(row, "mode", JsonValue(row, "label", ""));
        for (const char* field : {"model", "progress"}) {
          const std::string value = JsonValue(row, field, "");
          if (!value.empty()) text += " · " + value;
        }
        text += "\n";
      }
      return text;
    }
  }
  return JsonDump(result, 2) + "\n";
}

json ActivityCommand(AppSession& session, const ParsedSlashCommand& command) {
  std::istringstream input(command.argument);
  std::string target, operation, text;
  input >> target >> operation;
  std::getline(input, text);
  text = Trim(text);
  auto& processes = session.Runtime().processes;
  if (target.empty()) {
    return command.spec->id == SlashCommandId::kAgents
               ? json{{"collaborators", AgentsJson(session)}}
               : json{{"activities", processes.ActivityViews()}};
  }
  json request = {{"kind", "activity"},
                  {"operation", operation.empty() ? "inspect" : operation},
                  {"text", text}};
  if (operation == "output") request["operation"] = "inspect";
  if (command.spec->id == SlashCommandId::kAgents) {
    request["agent_id"] = target;
    for (const json& row : processes.ActivityViews()) {
      if (JsonValue(row, "agent_id", "") == target) {
        request["activity_id"] = row["id"];
      }
    }
  } else {
    int64_t id = 0;
    if (!ParseInt64(target.c_str(), id)) {
      return {{"error", "invalid activity id"}};
    }
    request["activity_id"] = id;
  }
  return SessionControl(session, request);
}

bool RunSlashCommand(AppSession& session, const ParsedSlashCommand& command,
                     json& result) {
  bool quit = false;
  switch (command.spec->id) {
    case SlashCommandId::kQuit:
      quit = true;
      break;
    case SlashCommandId::kReset:
      session.context.session_approvals.clear();
      session.ActiveAgent().Reset();
      session.context.observability.Journal().Clear();
      session.attachments.clear();
      session.session_file.clear();
      session.saved_revision = session.ActiveAgent().Revision();
      printf("%s· fresh session%s\n", DIM(), RST());
      break;
    case SlashCommandId::kSessions: {
      bool render = session.context.channel == nullptr;
      std::string chosen = PickSession(render);
      if (!chosen.empty()) {
        std::string previous_path = session.session_file;
        ResumeInto(session.ActiveAgent(), chosen, session.session_file, render);
        LoadSessionJournal(session, previous_path);
        session.attachments.clear();
        session.saved_revision = session.ActiveAgent().Revision();
      }
      break;
    }
    case SlashCommandId::kTrace:
      if (!command.argument.empty()) {
        size_t offset = 0;
        do {
          result = session.ActiveAgent().RawExchange(command.argument, offset);
          printf("%s", TerminalSafe(JsonValue(result, "text",
                                              JsonValue(result, "error", "")))
                           .c_str());
          offset = JsonValue(result, "next", size_t{0});
        } while (JsonValue(result, "more", false));
        printf("\n");
        fflush(stdout);
        return false;
      }
      if (session.context.channel == nullptr) {
        session.ActiveAgent().PrintTrace();
      }
      break;
    case SlashCommandId::kVariant:
      HandleVariant(session, command.argument);
      break;
    case SlashCommandId::kVerbose:
      session.ActiveAgent().SetVerbose(!session.ActiveAgent().Verbose());
      printf("%s· verbose %s%s\n", DIM(),
             session.ActiveAgent().Verbose()
                 ? "ON — full reasoning and expanded bounded tool output"
                 : "off — compact reasoning and compact tool output",
             RST());
      break;
    case SlashCommandId::kHelp:
      if (session.context.channel == nullptr) PrintCommandHelp();
      break;
    case SlashCommandId::kModels:
      HandleModels(session, command.argument);
      break;
    case SlashCommandId::kModel:
      HandleModel(session, command.argument);
      break;
    case SlashCommandId::kEffort:
      HandleEffort(session, command.argument);
      break;
    case SlashCommandId::kFork: {
      result = SessionControl(session,
                              {{"kind", "fork"}, {"title", command.argument}});
      if (!result.contains("error") &&
          session.Runtime().processes.Count() == 0) {
        std::string previous = session.session_file;
        if (ResumeInto(session.ActiveAgent(), JsonValue(result, "path", ""),
                       session.session_file, false)) {
          LoadSessionJournal(session, previous);
          session.Runtime().processes.SetOwner(HashHex(session.session_file));
          session.attachments.clear();
          result["continued"] = true;
        }
      } else if (!result.contains("error")) {
        result["note"] =
            "Fork saved. This terminal remains with its background activity; "
            "open the fork using /sessions.";
      }
      if (session.context.channel == nullptr) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
        fflush(stdout);
      }
      return false;
    }
    case SlashCommandId::kPermissions:
      result = PermissionControl(session.context, {{"mode", command.argument}});
      session.ActiveAgent().ApprovalChanged();
      if (session.context.channel == nullptr) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
        fflush(stdout);
      }
      return false;
    case SlashCommandId::kConfig: {
      json request = {{"kind", "config"}};
      if (!command.argument.empty()) {
        std::istringstream input(command.argument);
        std::string scope, change;
        input >> scope;
        std::getline(input, change);
        change = Trim(change);
        bool unset = change.starts_with("unset ");
        size_t equal = change.find('=');
        if (unset) change = Trim(change.substr(6));
        request.update(
            {{"operation", "apply"},
             {"scope", scope},
             {"changes",
              json::array({{{"key", unset ? change : change.substr(0, equal)},
                            {"value", unset || equal == std::string::npos
                                          ? ""
                                          : change.substr(equal + 1)},
                            {"unset", unset}}})}});
      }
      result = SessionControl(session, request);
      if (session.context.channel == nullptr) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
        fflush(stdout);
      }
      return false;
    }
    case SlashCommandId::kHttp: {
      auto exchanges = session.ActiveAgent().HttpExchanges();
      if (exchanges.empty()) {
        result = {{"error", "No HTTP exchange captured"}};
      } else {
        size_t index = exchanges.size();
        std::string part = "request";
        std::istringstream input(command.argument);
        if (!command.argument.empty()) input >> index >> part;
        if (index == 0 || index > exchanges.size() ||
            (part != "request" && part != "response")) {
          result = {{"error", "Use /http INDEX request|response"}};
        } else {
          result = exchanges[index - 1];
          if (session.context.channel == nullptr) {
            printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
            size_t offset = 0;
            for (;;) {
              auto page = ReadPrivateArtifact(
                  JsonValue(result, (part + "_path").c_str(), ""), offset);
              printf("%s", TerminalSafe(JsonValue(page, "text", JsonDump(page)))
                               .c_str());
              if (!JsonValue(page, "more", false)) break;
              offset = JsonValue(page, "next", offset);
            }
            printf("\n");
            fflush(stdout);
          }
          return false;
        }
      }
      if (session.context.channel == nullptr) {
        printf("%s\n", JsonDump(result).c_str());
      }
      return false;
    }
    case SlashCommandId::kYolo:
      result = PermissionControl(
          session.context, {{"mode", ApprovalIsAutomatic() ? "ask" : "yolo"}});
      session.ActiveAgent().ApprovalChanged();
      printf(
          "%s· yolo %s%s\n", DIM(),
          ApprovalIsAutomatic() ? "ON — automatic ordinary approvals" : "off",
          RST());
      break;
    case SlashCommandId::kCompact:
      HandleCompact(session);
      break;
    case SlashCommandId::kContext:
      HandleContext(session);
      break;
    case SlashCommandId::kCost:
      HandleCost(session);
      break;
    case SlashCommandId::kMemory:
    case SlashCommandId::kSkills:
    case SlashCommandId::kSchedule:
      result = ManagementCommand(
          command.spec->id == SlashCommandId::kMemory   ? "memory"
          : command.spec->id == SlashCommandId::kSkills ? "skills"
                                                        : "schedule",
          command.argument);
      if (!session.context.channel) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
      }
      return false;
    case SlashCommandId::kTools:
      HandleTools(session);
      break;
    case SlashCommandId::kStatus:
      HandleStatus(session);
      break;
    case SlashCommandId::kDebugConfig:
      HandleDebugConfig(session, command.argument);
      break;
    case SlashCommandId::kAttach:
      HandleAttach(session, command.argument);
      break;
    case SlashCommandId::kProcesses:
    case SlashCommandId::kAgents:
      result = ActivityCommand(session, command);
      if (session.context.channel == nullptr) {
        printf("%s", TerminalSafe(ActivityText(result)).c_str());
        fflush(stdout);
      }
      return false;
    case SlashCommandId::kDiff:
    case SlashCommandId::kInit:
    case SlashCommandId::kReview:
      // SlashCommandPrompt turns these into an ordinary turn before the
      // dispatcher ever sees them.
      break;
  }
  // Notices above are written with bare printf; the interactive composer owns
  // stdout and only sees what has left the buffer.
  fflush(stdout);
  result = CommandResult(session, command);
  return quit;
}

}  // namespace uagent
