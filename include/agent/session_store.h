// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
// Saved sessions on disk: the file header, listing and restoring them, and
// the lock that gives one process a session.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/file_watch.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/usage.h"

namespace uagent {

// Session-file header fields, shared with the picker's reader (ui/sessions.h).
inline constexpr const char* kSessionHeaderCwd = "cwd";
inline constexpr const char* kSessionHeaderModel = "model";
inline constexpr const char* kSessionHeaderSessionId = "session_id";
inline constexpr const char* kSessionHeaderTurns = "turns";
inline constexpr const char* kSessionHeaderTitle = "title";
inline constexpr const char* kSessionHeaderParent = "parent_session_id";
inline constexpr const char* kSessionHeaderForkTurn = "forked_at_turn";
inline constexpr const char* kSessionHeaderForkTime = "forked_at_time";
// Present only on an agent (AgentOf in session_role.h).
inline constexpr const char* kSessionHeaderAgent = "agent";
inline constexpr int64_t kSessionFormat = 3;
inline constexpr size_t kSessionHeaderBytes = size_t{16} * 1024;
inline constexpr size_t kSessionReadBytes = size_t{64} * 1024 * 1024;

// kWorkspace: this folder's own conversations, without agents.
enum class SessionScope { kWorkspace, kAll };

struct SessionInfo {
  std::string path, cwd, title, model, session_id;
  bool custom_title = false;
  int64_t turns = 0;
  uint64_t incoming = 0;
  double cost = 0;      // reported spend, as of the last save
  int64_t tokens = 0;   // every token its calls processed, as of the last save
  json todo = nullptr;  // how far its to-do list is (TodoProgress)
  int64_t bytes = 0;
  std::filesystem::file_time_type mtime;
  std::string error;
  std::string parent;  // the session id of whoever started it, for an agent
  bool memory = true;
  std::string host = {};
  json suggestion = nullptr;
};

// The suggestion of a conversation that was told to stay where it is.
inline json KeptSuggestion() { return {{"dismissed", true}}; }
// A suggestion (Agent::Suggestion) still waiting for its answer.
inline bool OpenSuggestion(const json& suggestion) {
  const json* options = JsonArray(suggestion, "options");
  return options && !options->empty() &&
         !JsonValue(suggestion, "dismissed", false);
}

// Every valid conversation saved in the directory of `path`, newest first.
std::vector<SessionInfo> SessionsBeside(const std::string& path);

// The session file of `parent` (a session id) beside `path`; empty for none.
std::string AgentParentPath(const std::string& path, const std::string& parent);

// The first line when it is a valid header, else {}; reads no transcript.
json SessionHeader(const std::string& path);

// The writer lease file that guards a session file.
std::string SessionLockPath(const std::string& path);

// Read-only: bounded headers, one known directory level, no links.

std::vector<SessionInfo> ListSessions(
    SessionScope scope = SessionScope::kWorkspace);

// Reuses headers while their file identity, size and timestamp match.
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
  std::string host = {};
  std::string model;
  std::string session_id;
  int64_t turns = 0;
  std::string title;
  bool custom_title = false;
  std::string parent_session_id{};
  int64_t forked_at_turn = 0;
  std::string forked_at_time{};
  json agent = json::object();
  // False for a conversation run with memory off: nothing learns from it.
  bool memory = true;
  json suggestion = nullptr;
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
  json display = json::object();
  // Ids of the latest mail taken into the transcript (see core/mailbox.h).
  json delivered_mail = json::array();

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

// The session file's path, hashed as spelled. MailboxIdFor resolves links
// first, so the two agree only for a path without them.
inline std::string SessionIdFor(const std::string& path) {
  return HashHex(path);
}

bool ValidSessionTitle(const std::string& title);
std::string InvalidSessionTitleMessage();

// What the last save wrote, to skip a save of the same bytes.
struct SavedFile {
  std::string path;
  size_t digest = 0;
  uintmax_t size = 0;
  std::filesystem::file_time_type modified;
};

class SessionStore {
 public:
  // Borrows `conversation` for the call; `saved` lets an unchanged save skip.
  static SessionStoreStatus Save(const std::string& path,
                                 const SessionRecord& record,
                                 const Conversation* conversation = nullptr,
                                 SavedFile* saved = nullptr);
  static SessionLoadResult Load(const std::string& path,
                                const std::string& expected_cwd);
  static SessionLoadResult Inspect(const std::string& path);
  // Whole, or cut before a user message, whose text returns as "prompt".
  static json Fork(const std::string& path, const std::string& title = "",
                   bool source_owned = false, int64_t fork_turn = 0,
                   const std::string& message_id = "",
                   const SessionRecord* snapshot = nullptr);
  // Markdown for /share, without system, internal or runtime-context messages.
  static std::string ShareMarkdown(const SessionRecord& record);
  // {shared, path} or {error}; the caller holds the session's lease.
  static json Share(const std::string& path);
  static SessionStoreStatus Rename(const std::string& path,
                                   const std::string& title);
  // Calls `visit` on every string of messages, archive and display.
  static void VisitStrings(SessionState& state,
                           const std::function<void(json&)>& visit);
  // Edits under the session's lease; saved only when `edit` returns true.
  static SessionStoreStatus Edit(
      const std::string& path, const std::function<bool(SessionRecord&)>& edit);
  static SessionStoreStatus Remove(const std::string& path,
                                   const std::string& draft_path = "");
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
