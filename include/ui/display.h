// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_DISPLAY_H_
#define UAGENT_INCLUDE_UI_DISPLAY_H_
// Terminal rendering for the REPL: model/route listings and the status line.

#include <algorithm>
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

// Token counts as the status row and /cost both spell them. Cache is separate
// so the status row can drop it independently when the terminal is narrow.
inline std::string TokenSummary(const Usage& usage) {
  return FmtCount(usage.input) + " in · " + FmtCount(usage.output) + " out";
}

inline std::string CacheSummary(const Usage& usage) {
  return usage.cache_read
             ? "cache " + std::to_string(usage.CacheHitPercent()) + "%"
             : std::string();
}

// Compact session metadata for the persistent composer. Keep the stable
// identity first; transient work state gets its own line while a turn runs.
struct StatusView {
  int64_t context_used = 0;
  // The active route in schema form, [provider/]model[:variant][:effort], so
  // the row shows a selection the user could paste back into --model.
  std::string model;
  // Only set when no provider scope was resolvable, where the bare model id
  // alone would not say where the request goes.
  std::string host;
  bool verbose = false;
  bool yolo = false;
  size_t attachments = 0;
  size_t background = 0;
};

// One ordered list of segments, rendered in place and dropped by priority when
// the terminal is too narrow. A second hand-maintained "cramped" spelling of
// the same row is how the two copies used to drift.
inline std::string StatusBar(const Api& api, const Usage& usage,
                             const StatusView& view) {
  struct Segment {
    int priority;  // higher is dropped first
    std::string text;
  };
  std::vector<Segment> segments;
  auto add = [&segments](int priority, std::string text) {
    if (!text.empty()) segments.push_back({priority, std::move(text)});
  };

  add(0, view.host.empty() ? view.model : view.model + " @ " + view.host);
  std::string context = "ctx " + FmtCount(view.context_used);
  if (api.ctx_window > 0) context += "/" + FmtCount(api.ctx_window);
  add(1, std::move(context));
  if (usage.input || usage.output) add(4, TokenSummary(usage));
  add(5, CacheSummary(usage));
  if (usage.cost > 0) add(2, FmtCost(usage.cost));
  if (view.background) add(3, "bg:" + std::to_string(view.background));
  if (view.attachments) {
    add(3, std::to_string(view.attachments) + " attached");
  }
  if (view.verbose) add(6, "verbose");
  add(2, view.yolo ? "YOLO" : std::string());

  auto join = [&segments] {
    std::string line;
    for (const Segment& segment : segments) {
      if (!line.empty()) line += " · ";
      line += segment.text;
    }
    return line;
  };
  std::string line = join();
  if (!g_tty) return line;
  // Drop the least valuable segment until the row fits; PrintStatusBar still
  // performs the final UTF-8-safe clipping.
  while (DisplayWidth(line) > TerminalWidth(1) && segments.size() > 1) {
    auto victim = std::max_element(
        segments.begin(), segments.end(),
        [](const Segment& a, const Segment& b) {
          return a.priority < b.priority;
        });
    if (victim->priority == 0) break;
    segments.erase(victim);
    line = join();
  }
  return line;
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
