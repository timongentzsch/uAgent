// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_STORE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/json.h"
#include "include/core/usage.h"

namespace uagent {

// Session-file header field names (written and validated by SessionStore in
// session_store.cc; ui/sessions.h reads a lenient subset of the same fields
// for the resume picker). Shared here so the two views cannot drift.
inline constexpr const char* kSessionHeaderCwd = "cwd";
inline constexpr const char* kSessionHeaderModel = "model";
inline constexpr const char* kSessionHeaderSessionId = "session_id";
inline constexpr const char* kSessionHeaderTurns = "turns";
inline constexpr const char* kSessionHeaderTitle = "title";
inline constexpr const char* kSessionHeaderParent = "parent_session_id";
inline constexpr const char* kSessionHeaderForkTurn = "forked_at_turn";
inline constexpr const char* kSessionHeaderForkTime = "forked_at_time";
// Optional lineage: empty/zero when this session was never forked. Unknown
// to older readers, which ignore extra header fields.
inline constexpr int64_t kSessionFormat = 3;
inline constexpr size_t kSessionHeaderBytes = size_t{16} * 1024;
inline constexpr size_t kSessionReadBytes = size_t{64} * 1024 * 1024;

enum class SessionScope { kWorkspace, kAll };

struct SessionInfo {
  std::string path, cwd, title;
  int64_t turns = 0;
  uint64_t incoming = 0;
  int64_t bytes = 0;
  std::filesystem::file_time_type mtime;
  std::string error;
};

// A read-only catalogue: bounded headers, one known directory level, no links.
std::vector<SessionInfo> ListSessions(
    SessionScope scope = SessionScope::kWorkspace);

enum class SessionStoreError {
  kNone,
  kNotFound,
  kCorrupt,
  kIncompatible,
  kWrongWorkspace,
  kInvalid,
  kIo,
};

struct SessionMetadata {
  std::string cwd;
  std::string model;
  std::string session_id;
  int64_t turns = 0;
  std::string title;
  bool custom_title = false;
  std::string parent_session_id;
  int64_t forked_at_turn = 0;
  std::string forked_at_time;
};

struct SessionState {
  json messages = json::array();
  std::vector<MessageKind> message_kinds;
  json archive = json::array();
  int64_t archive_dropped_segments = 0;
  int64_t context_tokens = 0;
  Usage usage;
  RouteUsage route_usage;
  std::string last_sent_prompt;
  std::string adaptive_system;
  std::string adaptive_system_mode = "overlay";
  uint64_t adaptive_system_revision = 0;
  // Rendered tool receipts keyed by call id, so a resumed transcript can
  // redraw a diff instead of a grey summary line.
  json tool_displays = json::object();
  json display = json::object();
};

struct SessionRecord {
  SessionMetadata metadata;
  SessionState state;
};

struct SessionStoreStatus {
  SessionStoreError error = SessionStoreError::kNone;
  std::string message;

  bool Ok() const { return error == SessionStoreError::kNone; }
};

struct SessionLoadResult {
  SessionStoreStatus status;
  std::optional<SessionRecord> record;
};

bool ValidSessionTitle(const std::string& title);

class SessionStore {
 public:
  static SessionStoreStatus Save(const std::string& path,
                                 const SessionRecord& record);
  static SessionLoadResult Load(const std::string& path,
                                const std::string& expected_cwd);
  static SessionLoadResult Inspect(const std::string& path);
  static json Fork(const std::string& path, const std::string& title = "",
                   bool source_owned = false, int64_t fork_turn = 0);
  // Truncates the saved session before its Nth user turn, keeping the same
  // identity and title. Records a reset-boundary display fact so the cut
  // stays visible after the dropped messages are gone.
  static json Rewind(const std::string& path, int64_t turn);
  // Renders the saved session as markdown for /share. Pure transcript view:
  // user and assistant text plus truncated tool results; system, internal
  // and runtime-context messages never leave the session file.
  static std::string ShareMarkdown(const SessionRecord& record);
  // Writes ShareMarkdown next to the session file and returns the sibling
  // path ({{"shared", true}, {"path", ...}}) or {{"error", ...}}.
  static json Share(const std::string& path);
  static SessionStoreStatus Rename(const std::string& path,
                                   const std::string& title);
  static SessionStoreStatus Remove(const std::string& path,
                                   const std::string& draft_path = "");
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
