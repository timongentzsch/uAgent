// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_SESSION_STORE_H_
#define UAGENT_INCLUDE_AGENT_SESSION_STORE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/conversation.h"
#include "include/core/json.h"
#include "include/core/usage.h"

namespace uagent {

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
  json checkpoint_candidates = json::array();
  json pending_checkpoint = nullptr;
  json side_effects = json::array();
  int64_t context_tokens = 0;
  Usage usage;
  RouteUsage route_usage;
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
