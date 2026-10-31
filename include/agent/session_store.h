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
// Present only on an agent: who started it and how (AgentOf in
// session_role.h).
inline constexpr const char* kSessionHeaderAgent = "agent";
// Optional lineage: empty/zero when this session was never forked. Unknown
// to older readers, which ignore extra header fields.
inline constexpr int64_t kSessionFormat = 3;
inline constexpr size_t kSessionHeaderBytes = size_t{16} * 1024;
inline constexpr size_t kSessionReadBytes = size_t{64} * 1024 * 1024;

// kWorkspace: this folder's own conversations, which `-c` and resume pick
// from: no agent, which is opened from its parent.
enum class SessionScope { kWorkspace, kAll };

struct SessionInfo {
  std::string path, cwd, title, model;
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
};

// Every valid conversation saved in the directory of `path`, it included,
// newest first: a folder's own and the agents its conversations started,
// which are saved beside their parent wherever they run.
std::vector<SessionInfo> SessionsBeside(const std::string& path);

// The session file, beside the agent at `path`, of its parent `parent` (a
// session id), or empty when there is none (any more).
std::string AgentParentPath(const std::string& path, const std::string& parent);

// The first line of a session file when it is a valid header, else an empty
// object. Bounded: it never reads the transcript.
json SessionHeader(const std::string& path);

// A read-only catalogue: bounded headers, one known directory level, no links.
// The writer lease file that guards a session file.
std::string SessionLockPath(const std::string& path);

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
  json agent = json::object();
  // False for a conversation run with memory off: nothing learns from it.
  bool memory = true;
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
  // Ids of the latest mailbox messages taken into the transcript, so one
  // delivered again after a crash is recognised (see core/mailbox.h).
  json delivered_mail = json::array();

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

// A session's id as clients and runtimes name it: its session file's path,
// hashed as spelled. MailboxIdFor resolves links first, so the two agree only
// for a path without them.
inline std::string SessionIdFor(const std::string& path) {
  return HashHex(path);
}

bool ValidSessionTitle(const std::string& title);
// What the rule is, for the error that rejects a title.
std::string InvalidSessionTitleMessage();

// What the last save wrote, so a save of the same bytes to the same untouched
// file writes nothing.
struct SavedFile {
  std::string path;
  size_t digest = 0;
  uintmax_t size = 0;
  std::filesystem::file_time_type modified;
};

class SessionStore {
 public:
  // A live conversation is borrowed only for this synchronous serialization.
  // With `saved`, a save whose bytes match the last one, on a file nobody has
  // touched since, leaves the file alone.
  static SessionStoreStatus Save(const std::string& path,
                                 const SessionRecord& record,
                                 const Conversation* conversation = nullptr,
                                 SavedFile* saved = nullptr);
  static SessionLoadResult Load(const std::string& path,
                                const std::string& expected_cwd);
  static SessionLoadResult Inspect(const std::string& path);
  // Copies the session, whole or before a user message: the Nth
  // (`fork_turn`) or the one shown as `message_id` ("m-<id>"). A cut fork
  // returns that message's text as "prompt", so it can be edited and sent.
  // An owning runtime may pass a published snapshot instead of reading its
  // on-disk checkpoint; asset copying and lineage still follow the same path.
  static json Fork(const std::string& path, const std::string& title = "",
                   bool source_owned = false, int64_t fork_turn = 0,
                   const std::string& message_id = "",
                   const SessionRecord* snapshot = nullptr);
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
  // Calls `visit` on every string of the transcript: messages, archive and
  // display.
  static void VisitStrings(SessionState& state,
                           const std::function<void(json&)>& visit);
  // Changes the saved session at `path` under its lease. `edit` returns
  // whether it changed the record; one that did not is not saved.
  static SessionStoreStatus Edit(
      const std::string& path, const std::function<bool(SessionRecord&)>& edit);
  static SessionStoreStatus Remove(const std::string& path,
                                   const std::string& draft_path = "");
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
