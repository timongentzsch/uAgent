// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// Terminal rendering for the REPL: path abbreviation, the model and
// route listings, completion setup, and the status line.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/api.h"
#include "include/cli.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/providers.h"

namespace uagent {

// abbreviate $HOME to ~ for display
inline std::string Tilde(const std::string& path) {
  std::string home = UserHome();
  if (!home.empty() && path.starts_with(home)) {
    return "~" + path.substr(home.size());
  }
  return path;
}

inline std::string ApiHost(const std::string& base_url) {
  return UrlHost(base_url);
}

#if defined(HAVE_EDITLINE)
inline void ConfigureReadlineCompletion(const std::vector<ModelRoute>& routes) {
  std::vector<std::string> models, efforts{"default"};
  for (const ModelRoute& route : routes) models.push_back(route.name);
  efforts.insert(efforts.end(), std::begin(kReasoningEfforts),
                 std::end(kReasoningEfforts));
  ConfigureCompletion(models, efforts);
}
#endif

inline void PrintModelRoutes(const std::vector<ModelRoute>& routes,
                             const Api& api) {
  if (routes.empty()) {
    printf("%s* %-20s %s @ %s%s\n", BOLD(), api.model.c_str(),
           api.model.c_str(), ApiHost(api.base_url).c_str(), RST());
    return;
  }
  for (const ModelRoute& route : routes) {
    bool active = route.base_url == api.base_url && route.model == api.model &&
                  route.effort == api.reasoning_effort;
    printf("%s%c %-20s %s", active ? BOLD() : DIM(), active ? '*' : ' ',
           route.name.c_str(), route.model.c_str());
    if (!route.effort.empty()) printf(" · %s", route.effort.c_str());
    printf(" @ %s%s\n", ApiHost(route.base_url).c_str(), RST());
  }
}

inline void PrintAvailableModels(Api& api, std::string filter) {
  printf("%s· querying %s/models…%s\n", DIM(), ApiHost(api.base_url).c_str(),
         RST());
  std::optional<std::vector<ModelInfo>> models =
      QueryModels(api, std::move(filter));
  if (!models) {
    printf("%s· model catalog unavailable%s\n", RED(), RST());
    return;
  }
  for (const ModelInfo& model : *models) {
    bool active = model.id == api.model;
#if defined(HAVE_EDITLINE)
    RegisterCompletion(CommandCompletion::kModels, model.id);
#endif
    if (active && model.context > 0) {
      api.ctx_window = model.context;
      setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
    }
    printf("%s%c %s", active ? BOLD() : DIM(), active ? '*' : ' ',
           model.id.c_str());
    if (model.context > 0) {
      printf(" · ctx %s", FmtTokens(model.context).c_str());
    }
    if (!model.efforts.empty()) {
      printf(" · effort ");
      for (size_t i = 0; i < model.efforts.size(); ++i) {
        printf("%s%s", i ? "," : "", model.efforts[i].c_str());
      }
      if (!model.default_effort.empty()) {
        printf(" (default %s)", model.default_effort.c_str());
      }
    }
    printf("%s\n", RST());
  }
  printf("%s· %zu model%s%s\n", DIM(), models->size(),
         models->size() == 1 ? "" : "s", RST());
}

// Prompt metadata stays in normal scrollback rather than a pinned TUI region.
inline std::string StatusBar(const Api& api, const Agent& agent, bool yolo,
                             size_t attachments,
                             const ProcessSupervisor& processes) {
  const Usage& u = agent.SessionUsage();
  std::string host = ApiHost(api.base_url);
  int64_t used = agent.ContextUsed();
  std::string s = api.model + " @ " + host + " · ctx " + FmtTokens(used);
  if (api.ctx_window > 0) {
    int64_t pct = (used * 100 + api.ctx_window - 1) / api.ctx_window;
    s += "/" + FmtTokens(api.ctx_window) + " (" + std::to_string(pct) + "%)";
  }
  if (!api.reasoning_effort.empty()) s += " · effort " + api.reasoning_effort;
  if (u.cache_read) s += " · cache " + FmtTokens(u.cache_read) + " total";
  if (u.cost > 0) s += " · spent " + FmtCost(u.cost);
  if (processes.PendingCount()) {
    s += " · bg:";
    for (pid_t pid : processes.PendingPids()) s += " " + std::to_string(pid);
  }
  size_t terminals = processes.DetachedCount();
  if (terminals) s += " · terminals:" + std::to_string(terminals);
  if (yolo) s += " · YOLO";
  if (attachments) s += " · " + std::to_string(attachments) + " attached";
  return s;
}

inline void PrintStatusBar(const std::string& status) {
  std::string safe = TerminalSafe(status);
  if (!g_tty) {
    printf("%s\n", safe.c_str());
    return;
  }
  safe = TerminalFit(safe);
  printf("%s%s\033[K%s\n", PanelMuted(), safe.c_str(), RST());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_DISPLAY_H_
