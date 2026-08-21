// Copyright 2026 Timon Gentzsch

#include "include/app/commands.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/ui/sessions.h"

namespace uagent {

void LoadSessionJournal(AppSession& session, const std::string& previous_path) {
  if (session.session_file.empty() || session.session_file == previous_path) {
    return;
  }
  std::string error;
  if (!session.context.observability.Journal().Load(
          session.session_file + ".events.jsonl", error)) {
    fprintf(stderr, "cannot load session journal: %s\n", error.c_str());
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
                    .yolo = session.context.options.yolo,
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
  printf(
      "%s· model %s%s\n", DIM(),
      RouteSelection(session.ApiClient(), session.context.provider.providers).c_str(),
      RST());
  if (!saved) {
    printf("%s· model changed but preference was not saved: %s%s\n", YEL(),
           TerminalSafe(error).c_str(), RST());
  }
}

// The interactive catalog picker: prints the matches, then reads a choice.
// Its only caller is /models, so it stays here rather than in a header.
std::optional<ModelCandidate> PickModel(ModelSearch search, Api& api) {
  std::string current = RouteSelection(api, {});
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
      for (size_t effort = 0; effort < candidate.info.efforts.size();
           ++effort) {
        printf("%s%s", effort ? "," : "",
               candidate.info.efforts[effort].c_str());
      }
    }
    printf("%s\n", RST());
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
      "model # (blank/Esc keeps " + TerminalSafe(current) + "): ", cancelled,
      eof);
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
  TerminalSpinner spinner(true, SpinnerLabel("searching model catalogs"));
  ModelSearch search =
      SearchModels(session.ApiClient(), session.context.provider.routes,
                   session.context.provider.providers, argument);
  spinner.Stop();
  if (AbortRequested()) {
    ClearAbort();
    printf("%s· model search cancelled%s\n", YEL(), RST());
    return;
  }
  std::optional<ModelCandidate> selected = PickModel(search, session.ApiClient());
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
  std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, argument);
  if (selected.empty()) {
    printf("%s· unknown model %s; use /models%s\n", RED(),
           TerminalSafe(argument).c_str(), RST());
    return;
  }
  SaveSelectedModel(session, selected);
}

void HandleEffort(AppSession& session, const std::string& argument) {
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
  } else if (!ValidEffort(argument)) {
    printf(
        "%s· effort must be none, minimal, low, medium, high, xhigh, or "
        "max; use default to defer to the provider%s\n",
        RED(), RST());
  } else {
    session.ApiClient().reasoning_effort = argument;
    ActivateCurrentRoute(session);
    printf("%s· effort %s%s\n", DIM(), argument.c_str(), RST());
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
    std::string label = session.ApiClient().config.openrouter_variant.empty()
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
  DebugLog("variant_changed",
           {{"variant", variant}, {"model", session.ApiClient().RequestModel()}});
  std::string label = variant.empty() ? "default" : ":" + variant;
  printf("%s· variant %s — %s%s\n", DIM(), label.c_str(), detail, RST());
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
      !(error =
            ImageInputError(attachment, session.ApiClient().capabilities.image_input,
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
  const json& sources = effective["sources"];
  auto source = [&](const char* key, std::string fallback = "runtime") {
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
      {"credentials",
       session.ApiClient().api_key.empty() || session.ApiClient().api_key == "sk-noop"
           ? "<unset>"
           : "<set>"},
      {"credential_source", std::move(credential_source)},
      {"context_window", session.ApiClient().ctx_window}};
  printf("%seffective configuration%s\n%s\n", BOLD(), RST(),
         TerminalSafe(JsonDump(effective, 2)).c_str());
  printf("%smodel request%s\n", BOLD(), RST());
  session.ActiveAgent().PrintContext();
}

// The startup row is a snapshot; MCP refresh and config reloads change the
// set mid-session, so this is the live view.

void HandleTools(const AppSession& session) {
  const std::vector<Tool>& tools = session.context.tools;
  printf("%s· %zu tools%s\n", DIM(), tools.size(), RST());
  for (const Tool& tool : tools) {
    printf("%s· %s%s\n", DIM(), TerminalSafe(tool.name).c_str(), RST());
  }
}

void HandleMemory(const AppSession& session) {
  printf("%s· memory %s%s\n", DIM(),
         session.ApiClient().config.memory_enabled ? "on" : "off", RST());
  if (!session.ApiClient().config.memory_enabled) return;
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

void HandleProcesses(const AppSession& session) {
  ToolResult activities = ToolActivityList(session.Runtime().processes);
  if (activities.output.starts_with('(')) {
    printf("%s· %s%s\n", DIM(), activities.output.c_str(), RST());
    return;
  }
  printf("%sbackground work%s\n%s%s%s", BOLD(), RST(), DIM(),
         TerminalSafe(activities.output).c_str(), RST());
}

}  // namespace

bool RunSlashCommand(AppSession& session, const ParsedSlashCommand& command) {
  switch (command.spec->id) {
    case SlashCommandId::kQuit:
      return true;
    case SlashCommandId::kReset:
      session.ActiveAgent().Reset();
      session.context.observability.Journal().Clear();
      session.attachments.clear();
      session.session_file.clear();
      session.saved_revision = session.ActiveAgent().Revision();
      printf("%s· fresh session%s\n", DIM(), RST());
      break;
    case SlashCommandId::kSessions: {
      std::string chosen = PickSession();
      if (!chosen.empty()) {
        std::string previous_path = session.session_file;
        ResumeInto(session.ActiveAgent(), chosen, session.session_file);
        LoadSessionJournal(session, previous_path);
        session.attachments.clear();
        session.saved_revision = session.ActiveAgent().Revision();
      }
      break;
    }
    case SlashCommandId::kTrace:
      session.ActiveAgent().PrintTrace();
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
      PrintCommandHelp();
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
    case SlashCommandId::kYolo:
      session.context.options.yolo = !session.context.options.yolo;
      printf("%s· yolo %s%s\n", DIM(),
             session.context.options.yolo ? "ON — auto-approving everything"
                                          : "off",
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
      HandleMemory(session);
      break;
    case SlashCommandId::kTools:
      HandleTools(session);
      break;
    case SlashCommandId::kAttach:
      HandleAttach(session, command.argument);
      break;
    case SlashCommandId::kProcesses:
      HandleProcesses(session);
      break;
  }
  // Notices above are written with bare printf; the interactive composer owns
  // stdout and only sees what has left the buffer.
  fflush(stdout);
  return false;
}

}  // namespace uagent
