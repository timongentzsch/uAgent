// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_STORE_H_

#include <cstdint>
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
inline constexpr int64_t kSessionFormat = 3;
// Format 2 predates `message_kinds`; the loader reconstructs them from the
// roles so an older session still resumes and still feeds memory extraction.
inline constexpr int64_t kOldestReadableSessionFormat = 2;

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
};

struct SessionState {
  json messages = json::array();
  std::vector<MessageKind> message_kinds;
  json archive = json::array();
  int64_t archive_dropped_segments = 0;
  int64_t context_tokens = 0;
  Usage usage;
  RouteUsage route_usage;
  std::string adaptive_system;
  uint64_t adaptive_system_revision = 0;
  // Rendered tool receipts keyed by call id, so a resumed transcript can
  // redraw a diff instead of a grey summary line.
  json tool_displays = json::object();
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

class SessionStore {
 public:
  static SessionStoreStatus Save(const std::string& path,
                                 const SessionRecord& record);
  static SessionLoadResult Load(const std::string& path,
                                const std::string& expected_cwd);
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
