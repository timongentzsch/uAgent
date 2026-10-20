// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_PANEL_INTERNAL_H_
#define UAGENT_SRC_APP_PANEL_INTERNAL_H_
// Module-private split of the panel: panel.cc keeps a folder's board, notes
// and spend and assembles the tools, panel_history.cc reads the folder's
// sessions and panel_threads.cc starts, steers and decides for its threads.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent::panel {

ToolResult Unavailable(const std::string& error);
std::optional<SessionInfo> FindSession(const std::string& folder,
                                       const std::string& id);
// Runs `fn` on the session of the folder that `a` names; with `thread`, only
// on one of its threads.
template <typename Fn>
ToolResult WithSession(const std::string& folder, const json& a, Fn fn,
                       bool thread = false) {
  const std::string id = JsonValue(a, "session_id", "");
  auto info = FindSession(folder, id);
  if (!info || (thread && info->kind != kSessionKindThread)) {
    return ToolFailure(
        ToolErrorCode::kNotFound,
        (thread ? "no thread " : "no session ") + id + " in this folder");
  }
  return fn(*info);
}

// Up to `cap` bytes of a session's text, labelled as its data.
ToolResult SessionData(const SessionInfo& info, std::string text, size_t cap);
std::string Today();
bool OwnThread(const SessionInfo& info, const std::string& panel);
// A thread counts against the cap while its runtime is working a turn.
bool Working(const SessionInfo& info);
// Today's spend against the daily limit: the panel's own turns and its
// threads'. A thread flagged in `working` counts its whole budget, so
// threads running at once can never overshoot the limit.
double SpentToday(const std::string& folder,
                  const std::vector<SessionInfo>& threads,
                  const std::vector<bool>& working);
Tool HistoryTool(const std::string& folder);
Tool ThreadTool(const std::string& folder,
                std::function<std::string()> own_model);
Tool DecideTool(const std::string& folder);

}  // namespace uagent::panel

#endif  // UAGENT_SRC_APP_PANEL_INTERNAL_H_
