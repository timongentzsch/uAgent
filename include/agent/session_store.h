// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_STORE_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/file_watch.h"
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
// Present only on a delegated child: who spawned it and in what role
// (parent, name, description, directive, mode, model, route, label, memory).
// Children are addressed by their parent, never listed as sessions.
inline constexpr const char* kSessionHeaderDelegation = "delegation";
// Present only on a folder's coordinator ("coordinator") or on a session it
// launched ("thread"); absent means an ordinary session. A thread also
// carries its link: {coordinator_id, brief, ceiling}, fixed at spawn.
inline constexpr const char* kSessionHeaderKind = "kind";
inline constexpr const char* kSessionHeaderThread = "thread";
inline constexpr const char* kSessionKindCoordinator = "coordinator";
inline constexpr const char* kSessionKindThread = "thread";
// A thread's decision with this route is its coordinator's to answer.
inline constexpr const char* kRouteCoordinator = "coordinator";

// A pending decision or approval that waits on a person rather than on the
// thread's coordinator.
inline bool WaitsOnPerson(const json& pending) {
  return pending.is_object() &&
         JsonValue(pending, "route", "") != kRouteCoordinator;
}
// Optional lineage: empty/zero when this session was never forked. Unknown
// to older readers, which ignore extra header fields.
inline constexpr int64_t kSessionFormat = 3;
inline constexpr size_t kSessionHeaderBytes = size_t{16} * 1024;
inline constexpr size_t kSessionReadBytes = size_t{64} * 1024 * 1024;

// kChildren: this workspace's delegated children, which the other scopes skip.
// No scope lists a coordinator; it is addressed by its folder instead.
enum class SessionScope { kWorkspace, kAll, kChildren };

struct SessionInfo {
  std::string path, cwd, title;
  int64_t turns = 0;
  uint64_t incoming = 0;
  double cost = 0;  // reported spend, as of the last save
  int64_t bytes = 0;
  std::filesystem::file_time_type mtime;
  std::string error;
  json delegation = json::object();
  std::string kind;
  json thread = json::object();
};

// The one coordinator session file of a canonical folder.
std::string CoordinatorPath(const std::string& cwd);

// The first line of a session file when it is a valid header, else an empty
// object. Bounded: it never reads the transcript.
json SessionHeader(const std::string& path);

// A read-only catalogue: bounded headers, one known directory level, no links.
std::vector<SessionInfo> ListSessions(
    SessionScope scope = SessionScope::kWorkspace);

// Reuse valid headers while their file identity, size and timestamp match.
// One owner serializes scans; each scan prunes missing or unreadable entries.
class SessionCatalogue {
 public:
  std::vector<SessionInfo> List(SessionScope scope = SessionScope::kWorkspace);

 private:
  struct Entry {
    FileStamp stamp;
    SessionInfo info;
  };
  std::map<std::string, Entry> entries_;
};

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
  std::string parent_session_id{};
  int64_t forked_at_turn = 0;
  std::string forked_at_time{};
  json delegation = json::object();
  std::string kind{};
  json thread = json::object();
};

struct SessionState {
  json messages = json::array();
  std::vector<MessageKind> message_kinds{};
  json archive = json::array();
  int64_t archive_dropped_segments = 0;
  int64_t context_tokens = 0;
  int64_t context_window = 0;
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

  // Transfer the loaded transcript into its sole runtime owner.
  bool RestoreConversation(Conversation& conversation) &&;
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
  // A live conversation is borrowed only for this synchronous serialization.
  static SessionStoreStatus Save(const std::string& path,
                                 const SessionRecord& record,
                                 const Conversation* conversation = nullptr);
  static SessionLoadResult Load(const std::string& path,
                                const std::string& expected_cwd);
  static SessionLoadResult Inspect(const std::string& path);
  // Copies the session, whole or before a user message: the Nth
  // (`fork_turn`) or the one shown as `message_id` ("m-<id>"). A cut fork
  // returns that message's text as "prompt", so it can be edited and sent.
  static json Fork(const std::string& path, const std::string& title = "",
                   bool source_owned = false, int64_t fork_turn = 0,
                   const std::string& message_id = "");
  // Renders the saved session as markdown for /share. Pure transcript view:
  // user and assistant text plus truncated tool results; system, internal
  // and runtime-context messages never leave the session file.
  static std::string ShareMarkdown(const SessionRecord& record);
  // Writes ShareMarkdown next to the session file and returns the sibling
  // path ({{"shared", true}, {"path", ...}}) or {{"error", ...}}. The caller
  // owns the session: the live worker that just saved it holds its lease.
  static json Share(const std::string& path);
  static SessionStoreStatus Rename(const std::string& path,
                                   const std::string& title);
  static SessionStoreStatus Remove(const std::string& path,
                                   const std::string& draft_path = "");
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
