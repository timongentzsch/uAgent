// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// Terminal rendering for the REPL: model/route listings and the status line.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/api.h"
#include "include/cli.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/usage.h"
#include "include/providers.h"

namespace uagent {

inline std::optional<ModelCandidate> PickModel(ModelSearch search, Api& api) {
  std::string current = ModelLabel(api.model, api.reasoning_effort);
  for (size_t i = 0; i < search.matches.size(); ++i) {
    const ModelCandidate& candidate = search.matches[i];
    bool active = candidate.route.base_url == api.base_url &&
                  candidate.route.model == api.model;
    if (active) {
      current = ModelLabel(candidate.selection, api.reasoning_effort);
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

// Compact session metadata for the persistent composer. Keep the stable
// identity first; transient work state gets its own line while a turn runs.
struct StatusView {
  int64_t context_used = 0;
  bool verbose = false;
  bool yolo = false;
  size_t attachments = 0;
  size_t background = 0;
};

inline std::string StatusBar(const Api& api, const Usage& usage,
                             const StatusView& view) {
  std::string host = UrlHost(api.base_url);
  std::string s = ModelLabel(api.RequestModel(), api.reasoning_effort) + " @ " +
                  host + " · ctx " + FmtCount(view.context_used);
  if (usage.cache_read) {
    s += " · cache " + FmtCount(usage.cache_read) + " total";
  }
  if (usage.cost > 0) s += " · spent " + FmtCost(usage.cost);
  if (view.background) s += " · bg:" + std::to_string(view.background);
  if (view.verbose) s += " · verbose";
  if (view.yolo) s += " · YOLO";
  if (view.attachments) {
    s += " · " + std::to_string(view.attachments) + " attached";
  }
  if (g_tty && DisplayWidth(s) > TerminalWidth(1)) {
    // On a cramped terminal keep live state ahead of long path/route labels.
    // PrintStatusBar performs the final UTF-8-safe clipping.
    s = ModelLabel(api.RequestModel(), api.reasoning_effort) + " · ctx " +
        FmtCount(view.context_used);
    if (usage.cost > 0) s += " · " + FmtCost(usage.cost);
    if (view.yolo) s += " · YOLO";
    if (view.attachments) {
      s += " · " + std::to_string(view.attachments) + " attached";
    }
    if (view.background) {
      s += " · bg:" + std::to_string(view.background);
    }
    if (view.verbose) s += " · verbose";
  }
  return s;
}

// The pinned status row: dim, clipped to the terminal, and cleared to the
// right so a shorter line never leaves stale text behind.
inline std::string StatusBarLine(const std::string& status) {
  return std::string(RST()) + DIM() +
         DisplayTrunc(TerminalSafe(status), TerminalWidth(1)) + "\033[K" +
         RST();
}

inline void PrintStatusBar(const std::string& status) {
  if (!g_tty) {
    printf("%s\n", TerminalSafe(status).c_str());
    return;
  }
  printf("%s\n", StatusBarLine(status).c_str());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_DISPLAY_H_
