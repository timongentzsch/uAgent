// Copyright 2026 Timon Gentzsch

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/app/config_proposal.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/providers.h"
#include "src/app/commands_internal.h"

namespace uagent {

void ActivateCurrentRoute(AppSession& session) {
  ActivateRoute(session.ApiClient());
  session.ActiveAgent().RouteChanged();
}

// The route now in use is this conversation's choice: the model setting at
// its scope, kept with it.
void ChooseCurrentRoute(AppSession& session) {
  ActivateCurrentRoute(session);
  session.context.config_manager.ChooseForConversation(
      "UAGENT_MODEL",
      RouteSelection(session.ApiClient(), session.context.provider.providers));
}

std::string SaveSelectedModel(AppSession& session, const std::string& selected,
                              bool as_default) {
  CommandReply reply;
  ChooseCurrentRoute(session);
  const std::string route =
      RouteSelection(session.ApiClient(), session.context.provider.providers);
  DebugLog("route_changed", {{"route", selected},
                             {"model", session.ApiClient().model},
                             {"base_url", session.ApiClient().base_url},
                             {"effort", session.ApiClient().reasoning_effort},
                             {"default", as_default}});
  reply.Note(Tone::kNeutral, "model " + route);
  if (as_default) {
    // New conversations start on it too: the same setting, saved for all.
    const json saved = ConfigurationControl(
        {{"operation", "apply"},
         {"scope", "user"},
         {"changes",
          json::array({{{"key", "UAGENT_MODEL"}, {"value", route}}})}},
        session.context.config_manager,
        session.context.config_manager.ProjectTrusted());
    const std::string error = JsonValue(saved, "error", "");
    reply.Note(error.empty() ? Tone::kNeutral : Tone::kWarn,
               error.empty()
                   ? "also the model of new conversations"
                   : "not saved for new conversations: " + TerminalSafe(error));
  }
  return reply.output;
}

// The interactive catalog picker: prints the matches, then reads a choice.
// Its only caller is /models, so it stays here rather than in a header.
std::optional<ModelCandidate> PickModel(
    ModelSearch search, Api& api, const std::vector<NamedProvider>& providers,
    CommandReply& reply) {
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
        OverrideSetting("UAGENT_CONTEXT", std::to_string(api.ctx_window));
      }
    }
    reply.Print("%s[%zu]%s %s%c %s", BOLD(), i + 1, RST(),
                active ? BOLD() : DIM(), active ? '*' : ' ',
                TerminalSafe(candidate.selection).c_str());
    if (!candidate.info.name.empty() &&
        candidate.info.name != candidate.info.id &&
        candidate.info.name != candidate.selection) {
      reply.Print(" · %s", TerminalSafe(candidate.info.name).c_str());
    }
    std::string effort = active ? api.reasoning_effort : candidate.route.effort;
    if (effort.empty()) effort = candidate.info.default_effort;
    reply.Print(" · effort %s", effort.empty() ? "default" : effort.c_str());
    if (candidate.info.context > 0) {
      reply.Print(" · ctx %s", FmtCount(candidate.info.context).c_str());
    }
    std::string supported;
    for (const auto& value : candidate.info.efforts) {
      supported += (supported.empty() ? " · supports " : ",") + value;
    }
    reply.Print("%s", supported.c_str());
    reply.Print("%s\n", RST());
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
    option_label += supported;
    options.push_back({{"value", std::to_string(i + 1)},
                       {"route", std::move(route_label)},
                       {"label", std::move(option_label)},
                       {"active", active},
                       {"effort", effort},
                       {"context", candidate.info.context},
                       {"supported_efforts", candidate.info.efforts}});
  }
  for (const std::string& unavailable : search.unavailable) {
    reply.Note(Tone::kWarn, TerminalSafe(unavailable) + " catalog unavailable");
  }
  reply.Note(Tone::kNeutral, std::to_string(search.matches.size()) + " model" +
                                 (search.matches.size() == 1 ? "" : "s"));
  if (search.matches.empty()) return std::nullopt;

  bool cancelled = false;
  bool eof = false;
  std::string answer = ReadChoiceLine(
      {.kind = "model.select",
       .prompt = "model # (blank/Esc keeps " + TerminalSafe(current) + "): ",
       .options = std::move(options)},
      cancelled, eof);
  if (cancelled || eof || answer.empty()) {
    reply.Note(Tone::kNeutral, "keeping " + TerminalSafe(current));
    return std::nullopt;
  }
  int64_t selected = 0;
  if (!ParseInt64(answer.c_str(), selected) || selected < 1 ||
      selected > static_cast<int64_t>(search.matches.size())) {
    reply.Note(Tone::kWarn,
               "not a listed number; keeping " + TerminalSafe(current));
    return std::nullopt;
  }
  return std::move(search.matches[static_cast<size_t>(selected - 1)]);
}

void HandleModels(AppSession& session, const std::string& argument,
                  CommandReply& reply) {
  if (argument.empty()) {
    reply.Note(Tone::kNeutral,
               "use /models QUERY to search every provider, or /models all "
               "for the full catalog");
    return;
  }
  std::string suffix =
      argument == "all" ? "" : " for " + TerminalSafe(argument);
  reply.Note(Tone::kNeutral, "searching all model catalogs" + suffix);
  ModelSearch search =
      SearchModels(session.ApiClient(), session.context.provider.routes,
                   session.context.provider.providers, argument);
  if (AbortRequested()) {
    ClearAbort();
    reply.Note(Tone::kWarn, "model search cancelled");
    return;
  }
  if (search.matches.empty()) {
    // A dead sidecar (e.g. an unreachable local proxy) must not disguise a
    // plain non-match as an outage: only blame the catalogs when every
    // queried one failed.
    const bool catalogs_down =
        search.queried > 0 && search.unavailable.size() >= search.queried;
    Emit(NoticeEvent(PresentationStatus::kFailed,
                     catalogs_down ? "model catalogs are unavailable; check "
                                     "provider settings"
                                   : "no matching models"));
    if (!catalogs_down) {
      for (const std::string& unavailable : search.unavailable) {
        Emit(NoticeEvent(PresentationStatus::kWarned,
                         unavailable + " catalog unavailable"));
      }
    }
  }
  if (search.matches.size() > kModelPickerMatches) {
    search.matches.resize(kModelPickerMatches);
    Emit(NoticeEvent(PresentationStatus::kWarned,
                     "showing the first " +
                         std::to_string(kModelPickerMatches) +
                         " models; use /models QUERY to narrow the catalog"));
  }
  std::optional<ModelCandidate> selected =
      PickModel(std::move(search), session.ApiClient(),
                session.context.provider.providers, reply);
  if (!selected) return;
  ApplyRoute(session.ApiClient(), selected->route);
  reply.Print("%s",
              SaveSelectedModel(session, selected->selection, false).c_str());
}

void HandleModel(AppSession& session, const std::string& argument,
                 CommandReply& reply) {
  if (argument.empty()) {
    HandleModels(session, "", reply);
    return;
  }
  // "/model X --default" also saves it for new conversations.
  static constexpr std::string_view kDefault = " --default";
  const bool as_default = argument.ends_with(kDefault);
  const std::string wanted =
      Trim(as_default ? argument.substr(0, argument.size() - kDefault.size())
                      : argument);
  ModelSelection requested = ParseModelSelection(wanted);
  std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, wanted);
  if (selected.empty()) {
    reply.Note(Tone::kError,
               "unknown model " + TerminalSafe(wanted) + "; use /models");
    return;
  }
  if (!requested.effort.empty() &&
      requested.effort != session.ApiClient().reasoning_effort) {
    reply.Note(Tone::kWarn, "effort " + requested.effort +
                                " is not supported by this model; using " +
                                (session.ApiClient().reasoning_effort.empty()
                                     ? "provider default"
                                     : session.ApiClient().reasoning_effort));
  }
  reply.Print("%s", SaveSelectedModel(session, selected, as_default).c_str());
}

// /effort and /variant change the selection /model chose: the same setting
// at the same scope.
void HandleEffort(AppSession& session, const std::string& argument,
                  CommandReply& reply) {
  if (ValidEffort(argument)) {
    ProbeModel(session.ApiClient(), /*discover_efforts=*/true);
  }
  if (argument.empty()) {
    reply.Note(Tone::kNeutral,
               "effort " + (session.ApiClient().reasoning_effort.empty()
                                ? std::string("default")
                                : session.ApiClient().reasoning_effort));
  } else if (argument == "default") {
    session.ApiClient().reasoning_effort.clear();
    ChooseCurrentRoute(session);
    reply.Note(Tone::kNeutral, "effort provider default");
  } else if (!ValidEffort(argument)) {
    reply.Note(Tone::kError,
               "effort must be none, minimal, low, medium, high, xhigh, or "
               "max; use default to defer to the provider");
  } else if (!SupportsReasoningEffort(session.ApiClient(), argument)) {
    reply.Note(Tone::kError,
               "effort " + argument + " is not supported by the active model");
  } else {
    session.ApiClient().reasoning_effort = argument;
    ChooseCurrentRoute(session);
    reply.Note(Tone::kNeutral, "effort " + argument);
  }
}

void HandleVariant(AppSession& session, const std::string& argument,
                   CommandReply& reply) {
  if (!session.ApiClient().capabilities.model_variants) {
    reply.Note(Tone::kError, "/variant is unavailable on the active route");
    return;
  }
  std::string variant = argument;
  if (variant.starts_with(':')) variant.erase(0, 1);
  if (variant.empty()) {
    std::string label =
        session.ApiClient().config.openrouter_variant.empty()
            ? "default"
            : ":" + session.ApiClient().config.openrouter_variant;
    reply.Note(Tone::kNeutral, "variant " + label +
                                   " · choose default, nitro, floor, or "
                                   "exacto");
    return;
  }
  if (variant == "default") variant.clear();
  if (!ValidOpenRouterVariant(variant)) {
    reply.Note(Tone::kError,
               "variant must be default, nitro, floor, or exacto");
    return;
  }
  session.ApiClient().config.openrouter_variant = variant;
  session.Runtime().config.openrouter_variant = variant;
  OverrideSetting("UAGENT_OPENROUTER_VARIANT", variant);
  ChooseCurrentRoute(session);
  const char* detail = "provider default";
  if (variant == "nitro") detail = "highest throughput";
  if (variant == "floor") detail = "lowest price";
  if (variant == "exacto") detail = "quality-first tool reliability";
  DebugLog("variant_changed", {{"variant", variant},
                               {"model", session.ApiClient().RequestModel()}});
  std::string label = variant.empty() ? "default" : ":" + variant;
  reply.Note(Tone::kNeutral, "variant " + label + " — " + detail);
}

}  // namespace uagent
