// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_COMMAND_H_
#define UAGENT_INCLUDE_APP_SESSION_COMMAND_H_
// Typed worker commands. The host forwards control frames as JSON; the worker
// parses each frame once into a SessionCommand and switches on its kind, so a
// misspelled kind is one enumerator miss away from a compile error instead of
// a silent fallthrough to "unsupported command". Field extraction never
// fails: absent fields read as empty. Bodies live in
// src/app/session_command.cc.

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "include/core/json.h"

namespace uagent::session {

// Every command kind a client can send. The host runs close, guide and the
// saved-session kinds (create, delete, activate) itself; the rest are
// forwarded to the worker, which answers anything it does not run as
// "unsupported command".
enum class SessionCommandKind {
  kClose,
  kInterrupt,
  kReply,
  kSteer,
  kGuide,
  kRecall,
  kRename,
  kRefresh,
  kPermissions,
  kActivity,
  kTools,
  kModel,
  kConfig,
  kContext,
  kFork,
  kRewind,
  kShare,
  kSide,
  kPrompt,
  kSubmit,
  kCreate,
  kDelete,
  kActivate,
  kUnknown,
};

const char* SessionCommandKindName(SessionCommandKind kind);
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
  bool has_attachments = false;
  // Null when absent, matching JsonValue(command, "budget", json{}).
  json budget;
  json raw;
};

// Validates the envelope identity (session/generation/request shape) and
// extracts the fields. Returns false for frames this worker must drop
// silently: wrong session/generation, or a malformed request id. An
// unrecognized kind is NOT a parse failure — it flows through as kUnknown so
// the caller answers "unsupported command" with an outcome.
bool ParseSessionCommand(const json& command, const std::string& session_id,
                         const std::string& generation, SessionCommand& out,
                         std::string& error);

enum class ReceiptVerdict {
  kNew,     // First sight: caller processes the command.
  kReplay,  // Identical retry: caller re-sends `previous` and returns.
  kReject,  // Same id, different content: caller drops silently.
};

// Bounded request-id dedup for retried worker commands. The cap evicts the
// oldest completed receipt first; when every receipt is still pending the
// check refuses, applying backpressure instead of forgetting a live command.
class ReceiptLog {
 public:
  ReceiptVerdict Check(const json& command, const std::string& request,
                       json& previous);
  void Record(const json& frame);

 private:
  std::mutex mutex_;
  std::map<std::string, std::pair<json, json>> receipts_;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_COMMAND_H_
