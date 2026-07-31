// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// Terminal rendering for the REPL: path abbreviation, the model and
// route listings, completion setup, and the status line.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/api.h"
#include "include/cli.h"
#include "include/core/fs.h"
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

#if defined(HAVE_EDITLINE)
inline void ConfigureReadlineCompletion(const std::vector<ModelRoute>& routes) {
  std::vector<std::string> models, efforts{"default"};
  for (const ModelRoute& route : routes) models.push_back(route.name);
  efforts.insert(efforts.end(), std::begin(kReasoningEfforts),
                 std::end(kReasoningEfforts));
  ConfigureCompletion(models, efforts);
}
#endif

inline std::optional<ModelCandidate> PickModel(ModelSearch search, Api& api) {
  std::string current = api.model;
  for (size_t i = 0; i < search.matches.size(); ++i) {
    const ModelCandidate& candidate = search.matches[i];
    bool active = candidate.route.base_url == api.base_url &&
                  candidate.route.model == api.model;
    if (active) {
      current = candidate.selection;
      if (candidate.info.context > 0) {
        api.ctx_window = candidate.info.context;
        setenv("UAGENT_CONTEXT", std::to_string(api.ctx_window).c_str(), 1);
      }
    }
#if defined(HAVE_EDITLINE)
    RegisterCompletion(CommandCompletion::kModels, candidate.selection);
#endif
    printf("%s[%zu]%s %s%c %s", CYAN(), i + 1, RST(), active ? BOLD() : DIM(),
           active ? '*' : ' ', TerminalSafe(candidate.selection).c_str());
    if (candidate.info.context > 0) {
      printf(" · ctx %s", FmtTokens(candidate.info.context).c_str());
    }
    if (!candidate.info.efforts.empty()) {
      printf(" · effort ");
      for (size_t effort = 0; effort < candidate.info.efforts.size();
           ++effort) {
        printf("%s%s", effort ? "," : "",
               candidate.info.efforts[effort].c_str());
      }
      if (!candidate.info.default_effort.empty()) {
        printf(" (default %s)", candidate.info.default_effort.c_str());
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
  char* end = nullptr;
  int64_t selected = strtoll(answer.c_str(), &end, 10);
  if (!end || *end != '\0' || selected < 1 ||
      selected > static_cast<int64_t>(search.matches.size())) {
    printf("%s· not a listed number; keeping %s%s\n", YEL(),
           TerminalSafe(current).c_str(), RST());
    return std::nullopt;
  }
  return std::move(search.matches[static_cast<size_t>(selected - 1)]);
}

// Compact session metadata for the prompt header.
inline std::string StatusBar(const Api& api, const Agent& agent, bool yolo,
                             size_t attachments,
                             const ProcessSupervisor& processes) {
  const Usage& u = agent.SessionUsage();
  std::string host = UrlHost(api.base_url);
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
  if (agent.Verbose()) s += " · verbose";
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
  printf("%s%s\033[K%s\n", DIM(), safe.c_str(), RST());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_DISPLAY_H_
