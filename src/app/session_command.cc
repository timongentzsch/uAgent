// Copyright 2026 Timon Gentzsch

#include "include/app/session_command.h"

#include <algorithm>
#include <string>
#include <utility>

#include "include/app/session.h"

namespace uagent::session {

namespace {

struct KindRow {
  std::string_view name;
  SessionCommandKind kind;
  bool forwarded;
};

constexpr KindRow kKinds[] = {
    {"close", SessionCommandKind::kClose, false},
    {"interrupt", SessionCommandKind::kInterrupt, true},
    {"reply", SessionCommandKind::kReply, true},
    {"steer", SessionCommandKind::kSteer, true},
    {"guide", SessionCommandKind::kGuide, false},
    {"recall", SessionCommandKind::kRecall, true},
    {"rename", SessionCommandKind::kRename, true},
    {"refresh", SessionCommandKind::kRefresh, true},
    {"permissions", SessionCommandKind::kPermissions, true},
    {"activity", SessionCommandKind::kActivity, true},
    {"tools", SessionCommandKind::kTools, true},
    {"model", SessionCommandKind::kModel, true},
    {"config", SessionCommandKind::kConfig, true},
    {"context", SessionCommandKind::kContext, true},
    {"fork", SessionCommandKind::kFork, true},
    {"rewind", SessionCommandKind::kRewind, true},
    {"share", SessionCommandKind::kShare, true},
    {"prompt", SessionCommandKind::kPrompt, true},
    {"submit", SessionCommandKind::kSubmit, true},
    {"create", SessionCommandKind::kCreate, false},
    {"delete", SessionCommandKind::kDelete, false},
    {"activate", SessionCommandKind::kActivate, false},
};

const KindRow* FindKind(SessionCommandKind kind) {
  for (const KindRow& row : kKinds) {
    if (row.kind == kind) return &row;
  }
  return nullptr;
}

}  // namespace

const char* SessionCommandKindName(SessionCommandKind kind) {
  const KindRow* row = FindKind(kind);
  return row ? row->name.data() : "";
}

SessionCommandKind ParseSessionCommandKind(std::string_view kind) {
  for (const KindRow& row : kKinds) {
    if (row.name == kind) return row.kind;
  }
  return SessionCommandKind::kUnknown;
}

bool ForwardsToWorker(SessionCommandKind kind) {
  const KindRow* row = FindKind(kind);
  return row && row->forwarded;
}

bool ParseSessionCommand(const json& command, const std::string& session_id,
                         const std::string& generation, SessionCommand& out,
                         std::string& error) {
  if (JsonValue(command, "session_id", "") != session_id ||
      JsonValue(command, "generation", "") != generation) {
    return false;
  }
  SessionCommand parsed;
  parsed.kind = ParseSessionCommandKind(JsonValue(command, "kind", ""));
  parsed.request_id = JsonValue(command, "request_id", "");
  if (!OpaqueId(parsed.request_id)) return false;
  parsed.text = JsonValue(command, "text", "");
  parsed.interaction_id = JsonValue(command, "interaction_id", "");
  parsed.client_request_id = JsonValue(command, "client_request_id", "");
  parsed.title = JsonValue(command, "title", "");
  parsed.operation = JsonValue(command, "operation", "");
  parsed.cancelled = JsonValue(command, "cancelled", false);
  parsed.has_attachments = command.contains("attachments");
  parsed.budget = JsonValue(command, "budget", json{});
  parsed.raw = command;
  out = std::move(parsed);
  error.clear();
  return true;
}

ReceiptVerdict ReceiptLog::Check(const json& command,
                                 const std::string& request, json& previous) {
  std::lock_guard lock(mutex_);
  auto found = receipts_.find(request);
  if (found != receipts_.end()) {
    if (found->second.first != command) return ReceiptVerdict::kReject;
    previous = found->second.second;
    return ReceiptVerdict::kReplay;
  }
  if (receipts_.size() >= kReceiptEntries) {
    auto completed =
        std::find_if(receipts_.begin(), receipts_.end(), [](const auto& item) {
          return !JsonValue(item.second.second, "pending", false);
        });
    if (completed == receipts_.end()) return ReceiptVerdict::kReject;
    receipts_.erase(completed);
  }
  receipts_[request] = {command,
                        {{"kind", "outcome"},
                         {"request_id", request},
                         {"accepted", true},
                         {"pending", true}}};
  return ReceiptVerdict::kNew;
}

void ReceiptLog::Record(const json& frame) {
  if (JsonValue(frame, "kind", "") != "outcome") return;
  std::lock_guard lock(mutex_);
  auto found = receipts_.find(JsonValue(frame, "request_id", ""));
  if (found != receipts_.end()) found->second.second = frame;
}

}  // namespace uagent::session
