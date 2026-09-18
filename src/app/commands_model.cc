// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/agent/process.h"
#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/commands.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/prompt_control.h"
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
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/tools/memory.h"
#include "include/tools/subagent.h"
#include "include/ui/conversation.h"
#include "include/ui/sessions.h"
#include "src/app/commands_internal.h"
namespace uagent {

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
    printf("%s[%zu]%s %s%c %s", BOLD(), i + 1, RST(), active ? BOLD() : DIM(),
           active ? '*' : ' ', TerminalSafe(candidate.selection).c_str());
    if (!candidate.info.name.empty() &&
        candidate.info.name != candidate.info.id &&
        candidate.info.name != candidate.selection) {
      printf(" · %s", TerminalSafe(candidate.info.name).c_str());
    }
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
    std::string option_label = candidate.selection;
    if (!candidate.info.name.empty() &&
        candidate.info.name != candidate.info.id) {
      option_label += " · " + candidate.info.name;
    }
    // The choice list (terminal TUI and web dropdown alike) renders labels
    // only, so the supported efforts ride along here, same vocabulary as
    // the printf rows below.
    if (!candidate.info.efforts.empty()) {
      option_label += " · supports ";
      for (size_t index = 0; index < candidate.info.efforts.size(); ++index) {
        option_label += (index ? "," : "") + candidate.info.efforts[index];
      }
    }
    options.push_back({{"value", std::to_string(i + 1)},
                       {"route", std::move(route_label)},
                       {"label", std::move(option_label)},
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
    // A dead sidecar (e.g. an unreachable local proxy) must not disguise a
    // plain non-match as an outage: only blame the catalogs when every
    // queried one failed.
    const bool catalogs_down =
        search.queried > 0 && search.unavailable.size() >= search.queried;
    Emit(NoticeEvent(PresentationStatus::kFailed,
                     catalogs_down ? "Model catalogs are unavailable. Check "
                                     "provider settings."
                                   : "No matching models."));
    if (!catalogs_down) {
      for (const std::string& unavailable : search.unavailable) {
        Emit(NoticeEvent(PresentationStatus::kWarned,
                         unavailable + " catalog unavailable"));
      }
    }
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

}  // namespace uagent
