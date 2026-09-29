// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_SESSIONS_H_
#define UAGENT_INCLUDE_UI_SESSIONS_H_
// The saved-session picker. One file per conversation under
// ~/.uagent/history, written by Agent::save as two lines: a cheap
// header, read here for the listing, and the full payload.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/cli.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/ui/conversation.h"
#include "include/ui/display.h"

namespace uagent {

// Unique prefix match over saved sessions: an exact path wins, otherwise the
// single session whose file name or title contains the argument
// (case-insensitive). Empty when absent or ambiguous, so callers fall back
// to the picker instead of guessing.
inline std::string MatchSessionPrefix(const std::string& prefix) {
  const std::string arg = AsciiLower(Trim(prefix));
  if (arg.empty()) return "";
  const std::vector<SessionInfo> sessions = ListSessions(SessionScope::kAll);
  for (const SessionInfo& session : sessions) {
    if (session.path == prefix) return session.path;
  }
  std::string match;
  for (const SessionInfo& session : sessions) {
    if (session.kind == kSessionKindCoordinator) continue;  // `uagent coord`
    const std::string name =
        std::filesystem::path(session.path).filename().string();
    if (AsciiLower(name).find(arg) == std::string::npos &&
        AsciiLower(session.title).find(arg) == std::string::npos) {
      continue;
    }
    if (!match.empty()) return "";  // Ambiguous: let the picker decide.
    match = session.path;
  }
  return match;
}

// print a numbered list and read a choice; returns the chosen path or "".
inline std::string PickSession(bool render = true) {
  std::vector<SessionInfo> sessions = ListSessions();
  if (sessions.empty()) {
    if (render) {
      fputs(Note(Tone::kNeutral, "no saved sessions").c_str(), stdout);
    }
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
      const std::string dot = AsciiGlyphs(" · ");
      printf("%s[%zu]%s %s%s%s%s%s turn%s%s%s%s%s\"%s\"%s\n", BOLD(), i + 1,
             RST(), FmtAgo(secs).c_str(), dot.c_str(),
             FmtBytes(s.bytes).c_str(), dot.c_str(), FmtCount(s.turns).c_str(),
             s.turns == 1 ? "" : "s", dot.c_str(), DIM(), safe_cwd.c_str(),
             dot.c_str(), safe_title.c_str(), RST());
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
  if (render) {
    fputs(Note(Tone::kNeutral, "not a listed number").c_str(), stdout);
  }
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
      fputs(Note(Tone::kError,
                 "could not resume " + safe_path + ": " + safe_error)
                .c_str(),
            stdout);
    }
    Emit(Event{EventId::kError, {{"error", "cannot resume: " + error}}});
    return false;
  }
  session_file = path;
  if (render) {
    fputs(Note(Tone::kNeutral, "resumed — " +
                                   std::to_string(agent.MessageCount() - 1) +
                                   " messages")
              .c_str(),
          stdout);
    PrintConversationHistory(agent.History());
    fputs(Note(Tone::kNeutral, "end of history, continuing").c_str(), stdout);
  }
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_SESSIONS_H_
