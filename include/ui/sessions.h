// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_SESSIONS_H_
#define UAGENT_INCLUDE_UI_SESSIONS_H_
// The saved-session picker. One file per conversation under
// ~/.uagent/history, written by Agent::save as two lines: a cheap
// header, read here for the listing, and the full payload.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/agent/session_store.h"
#include "include/cli.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/display.h"

namespace uagent {

// print a numbered list and read a choice; returns the chosen path or "".
inline std::string PickSession(bool render = true) {
  std::vector<SessionInfo> sessions = ListSessions();
  if (sessions.empty()) {
    if (render) printf("%s· no saved sessions%s\n", DIM(), RST());
    return "";
  }
  auto now = std::filesystem::file_time_type::clock::now();
  size_t shown = std::min(sessions.size(), size_t{20});
  json options = json::array();
  for (size_t i = 0; i < shown; ++i) {
    const SessionInfo& s = sessions[i];
    int64_t secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - s.mtime).count();
    std::string safe_cwd = TerminalSafe(Tilde(s.cwd));
    std::string safe_title = TerminalSafe(FirstLine(s.title));
    if (render) {
      printf("%s[%zu]%s %s · %s · %s turn%s · %s%s · \"%s\"%s\n", CYAN(), i + 1,
             RST(), FmtAgo(secs).c_str(), FmtBytes(s.bytes).c_str(),
             FmtCount(s.turns).c_str(), s.turns == 1 ? "" : "s", DIM(),
             safe_cwd.c_str(), safe_title.c_str(), RST());
    }
    options.push_back({{"value", std::to_string(i + 1)},
                       {"title", s.title},
                       {"cwd", s.cwd},
                       {"turns", s.turns},
                       {"bytes", s.bytes},
                       {"age_seconds", secs}});
  }
  bool cancelled = false;
  bool eof = false;
  std::string ans = ReadChoiceLine({.kind = "session.select",
                                    .prompt = "resume #: ",
                                    .options = std::move(options)},
                                   cancelled, eof);
  if (cancelled || eof || ans.empty()) return "";
  int64_t n = 0;
  if (ParseInt64(ans.c_str(), n) && n >= 1 &&
      n <= static_cast<int64_t>(shown)) {
    return sessions[static_cast<size_t>(n - 1)].path;
  }
  if (render) printf("%s· not a listed number%s\n", DIM(), RST());
  return "";
}

// load `path` into the agent; on success the session continues in that file
inline bool ResumeInto(Agent& agent, const std::string& path,
                       std::string& session_file, bool render = true) {
  if (path.empty()) return false;
  std::string error;
  if (!agent.Load(path, CanonicalCwd(), error)) {
    std::string safe_path = TerminalSafe(path);
    std::string safe_error = TerminalSafe(error);
    if (render) {
      printf("%s· could not resume %s: %s%s\n", RED(), safe_path.c_str(),
             safe_error.c_str(), RST());
    }
    Emit(Event{EventId::kError, {{"error", "cannot resume: " + error}}});
    return false;
  }
  session_file = path;
  if (render) {
    printf("%s· resumed — %zu messages%s\n", DIM(), agent.MessageCount() - 1,
           RST());
    agent.PrintHistory();
    printf("%s· end of history, continuing%s\n", DIM(), RST());
  }
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_SESSIONS_H_
