// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_COMMAND_H_
#define UAGENT_INCLUDE_APP_SESSION_COMMAND_H_
// Typed worker commands: the worker parses each control frame once into a
// SessionCommand. Absent fields read as empty.

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/thread_annotations.h"

namespace uagent::session {

// The host runs close and the saved-session kinds (create, delete, move,
// activate) itself and forwards the rest to the worker.
enum class SessionCommandKind {
  kClose,
  kInterrupt,
  kReply,
  kSteer,
  kRecall,
  kInsert,
  kRename,
  // Answers a suggested place with "here".
  kKeep,
  kRefresh,
  kPermissions,
  kActivity,
  kTools,
  kModel,
  kConfig,
  kContext,
  kRevert,
  kFork,
  kShare,
  kSide,
  kSubmit,
  // A parent hands its agent's routed decision to the person.
  kEscalate,
  kCreate,
  kDelete,
  kMove,
  kActivate,
  kUnknown,
};

SessionCommandKind ParseSessionCommandKind(std::string_view kind);
bool ForwardsToWorker(SessionCommandKind kind);

struct SessionCommand {
  SessionCommandKind kind = SessionCommandKind::kUnknown;
  std::string request_id;
  std::string text;
  std::string interaction_id;
  std::string client_request_id;
  std::string title;
  std::string operation;
  bool cancelled = false;
  bool from_parent = false;  // sent by the agent's parent
  // The whole command, for the fields only one handler reads.
  json raw;
};

// False for frames the worker drops silently: wrong session or generation,
// or a malformed request id. An unrecognized kind parses, as kUnknown.
bool ParseSessionCommand(const json& command, const std::string& session_id,
                         const std::string& generation, SessionCommand& out);

enum class ReceiptVerdict {
  kNew,     // First sight: caller processes the command.
  kReplay,  // Identical retry: caller re-sends `previous` and returns.
  kReject,  // Same id, different content: caller drops silently.
};

// Bounded request-id dedup for retried worker commands. The oldest completed
// receipt is evicted first; with every receipt pending, Check refuses.
class ReceiptLog {
 public:
  ReceiptVerdict Check(const json& command, const std::string& request,
                       json& previous);
  void Record(const json& frame);

 private:
  std::mutex mutex_;
  std::map<std::string, std::pair<json, json>> receipts_
      UAGENT_GUARDED_BY(mutex_);
  std::deque<std::string> order_ UAGENT_GUARDED_BY(mutex_);
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_COMMAND_H_
