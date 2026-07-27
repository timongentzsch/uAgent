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
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/cli.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/display.h"

namespace uagent {

// One file per conversation under ~/.uagent/history, written by Agent::save as
// two lines: a cheap header (read here for the picker) and the full payload.

struct SessionInfo {
  std::string path, cwd, title;
  int64_t turns = 0;
  std::filesystem::file_time_type mtime;
};

// newest first; malformed files still list, with a fallback title
inline std::vector<SessionInfo> ListSessions() {
  namespace fs = std::filesystem;
  std::vector<SessionInfo> out;
  std::error_code ec;
  std::string current = CanonicalCwd();
  std::string base = UagentDir(kHistoryDir);
  std::string scoped = base + "/" + WorkspaceId(current);
  fs::create_directories(scoped, ec);
  chmod(scoped.c_str(), 0700);
  for (const std::string& dir : {scoped, base}) {
    for (auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
      std::ifstream f(e.path());
      std::string head;
      std::getline(f, head);
      json h = json::parse(head, nullptr, false);
      if (!h.is_object() || JsonValue(h, "cwd", "") != current) continue;
      SessionInfo s;
      s.path = e.path().string();
      s.mtime = e.last_write_time(ec);
      s.cwd = JsonValue(h, "cwd", "");
      s.turns = JsonValue(h, "turns", int64_t{0});
      s.title = JsonValue(h, "title", "(untitled)");
      out.push_back(std::move(s));
    }
  }
  std::sort(out.begin(), out.end(),
            [](const SessionInfo& a, const SessionInfo& b) {
              return a.mtime > b.mtime;
            });
  return out;
}

// print a numbered list and read a choice; returns the chosen path or "".
inline std::string PickSession() {
  std::vector<SessionInfo> sessions = ListSessions();
  if (sessions.empty()) {
    printf("%s· no saved sessions%s\n", DIM(), RST());
    return "";
  }
  auto now = std::filesystem::file_time_type::clock::now();
  size_t shown = std::min(sessions.size(), size_t{20});
  for (size_t i = 0; i < shown; ++i) {
    const SessionInfo& s = sessions[i];
    int64_t secs =
        std::chrono::duration_cast<std::chrono::seconds>(now - s.mtime).count();
    std::string safe_cwd = TerminalSafe(Tilde(s.cwd));
    std::string safe_title = TerminalSafe(OneLine(s.title, 60));
    std::cout << CYAN() << '[' << i + 1 << ']' << RST() << ' ' << FmtAgo(secs)
              << " · " << s.turns << " turn" << (s.turns == 1 ? "" : "s")
              << " · " << DIM() << safe_cwd << " · \"" << safe_title << '"'
              << RST() << '\n';
  }
  bool eof = false;
  std::string ans =
      Trim(ReadInputLine("resume #: ", &eof, /*keep_history=*/false));
  if (eof || ans.empty()) return "";
  char* end = nullptr;
  int64_t n = strtol(ans.c_str(), &end, 10);
  if (end && *end == '\0' && n >= 1 && n <= static_cast<int64_t>(shown)) {
    return sessions[n - 1].path;
  }
  printf("%s· not a listed number%s\n", DIM(), RST());
  return "";
}

// load `path` into the agent; on success the session continues in that file
inline void ResumeInto(Agent& agent, const std::string& path,
                       std::string& session_file) {
  if (path.empty()) return;
  std::string error;
  if (!agent.Load(path, CanonicalCwd(), error)) {
    std::string safe_path = TerminalSafe(path);
    std::string safe_error = TerminalSafe(error);
    printf("%s· could not resume %s: %s%s\n", RED(), safe_path.c_str(),
           safe_error.c_str(), RST());
    return;
  }
  session_file = path;
  printf("%s· resumed — %zu messages%s\n", DIM(), agent.MessageCount() - 1,
         RST());
  agent.PrintHistory();
  printf("%s· end of history, continuing%s\n", DIM(), RST());
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_SESSIONS_H_
