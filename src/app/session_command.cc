// Copyright 2026 Timon Gentzsch

#include "include/app/session_command.h"

#include <algorithm>
#include <string>
#include <utility>

#include "include/app/session.h"

namespace uagent::session {

const char* SessionCommandKindName(SessionCommandKind kind) {
  switch (kind) {
    case SessionCommandKind::kClose:
      return "close";
    case SessionCommandKind::kInterrupt:
      return "interrupt";
    case SessionCommandKind::kReply:
      return "reply";
    case SessionCommandKind::kSteer:
      return "steer";
    case SessionCommandKind::kGuide:
      return "guide";
    case SessionCommandKind::kRecall:
      return "recall";
    case SessionCommandKind::kRename:
      return "rename";
    case SessionCommandKind::kRefresh:
      return "refresh";
    case SessionCommandKind::kPermissions:
      return "permissions";
    case SessionCommandKind::kActivity:
      return "activity";
    case SessionCommandKind::kModel:
      return "model";
    case SessionCommandKind::kConfig:
      return "config";
    case SessionCommandKind::kContext:
      return "context";
    case SessionCommandKind::kFork:
      return "fork";
    case SessionCommandKind::kRewind:
      return "rewind";
    case SessionCommandKind::kShare:
      return "share";
    case SessionCommandKind::kPrompt:
      return "prompt";
    case SessionCommandKind::kSubmit:
      return "submit";
    case SessionCommandKind::kUnknown:
      return "";
  }
  return "";
}

SessionCommandKind ParseSessionCommandKind(std::string_view kind) {
  if (kind == "close") return SessionCommandKind::kClose;
  if (kind == "interrupt") return SessionCommandKind::kInterrupt;
  if (kind == "reply") return SessionCommandKind::kReply;
  if (kind == "steer") return SessionCommandKind::kSteer;
  if (kind == "guide") return SessionCommandKind::kGuide;
  if (kind == "recall") return SessionCommandKind::kRecall;
  if (kind == "rename") return SessionCommandKind::kRename;
  if (kind == "refresh") return SessionCommandKind::kRefresh;
  if (kind == "permissions") return SessionCommandKind::kPermissions;
  if (kind == "activity") return SessionCommandKind::kActivity;
  if (kind == "model") return SessionCommandKind::kModel;
  if (kind == "config") return SessionCommandKind::kConfig;
  if (kind == "context") return SessionCommandKind::kContext;
  if (kind == "fork") return SessionCommandKind::kFork;
  if (kind == "rewind") return SessionCommandKind::kRewind;
  if (kind == "share") return SessionCommandKind::kShare;
  if (kind == "prompt") return SessionCommandKind::kPrompt;
  if (kind == "submit") return SessionCommandKind::kSubmit;
  return SessionCommandKind::kUnknown;
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
  if (receipts_.size() >= 256) {
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

HostCommandKind ParseHostCommandKind(std::string_view kind) {
  const SessionCommandKind worker = ParseSessionCommandKind(kind);
  switch (worker) {
    case SessionCommandKind::kClose:
      return HostCommandKind::kClose;
    case SessionCommandKind::kInterrupt:
      return HostCommandKind::kInterrupt;
    case SessionCommandKind::kReply:
      return HostCommandKind::kReply;
    case SessionCommandKind::kSteer:
      return HostCommandKind::kSteer;
    case SessionCommandKind::kGuide:
      return HostCommandKind::kGuide;
    case SessionCommandKind::kRecall:
      return HostCommandKind::kRecall;
    case SessionCommandKind::kRename:
      return HostCommandKind::kRename;
    case SessionCommandKind::kRefresh:
      return HostCommandKind::kRefresh;
    case SessionCommandKind::kPermissions:
      return HostCommandKind::kPermissions;
    case SessionCommandKind::kActivity:
      return HostCommandKind::kActivity;
    case SessionCommandKind::kModel:
      return HostCommandKind::kModel;
    case SessionCommandKind::kConfig:
      return HostCommandKind::kConfig;
    case SessionCommandKind::kContext:
      return HostCommandKind::kContext;
    case SessionCommandKind::kFork:
      return HostCommandKind::kFork;
    case SessionCommandKind::kRewind:
      return HostCommandKind::kRewind;
    case SessionCommandKind::kShare:
      return HostCommandKind::kShare;
    case SessionCommandKind::kPrompt:
      return HostCommandKind::kPrompt;
    case SessionCommandKind::kSubmit:
      return HostCommandKind::kSubmit;
    case SessionCommandKind::kUnknown:
      break;
  }
  if (kind == "create") return HostCommandKind::kCreate;
  if (kind == "delete") return HostCommandKind::kDelete;
  if (kind == "activate") return HostCommandKind::kActivate;
  return HostCommandKind::kUnknown;
}

bool ForwardsToWorker(HostCommandKind kind) {
  switch (kind) {
    case HostCommandKind::kSubmit:
    case HostCommandKind::kSteer:
    case HostCommandKind::kInterrupt:
    case HostCommandKind::kRecall:
    case HostCommandKind::kReply:
    case HostCommandKind::kRefresh:
    case HostCommandKind::kRename:
    case HostCommandKind::kModel:
    case HostCommandKind::kActivity:
    case HostCommandKind::kPermissions:
    case HostCommandKind::kConfig:
    case HostCommandKind::kContext:
    case HostCommandKind::kFork:
    case HostCommandKind::kRewind:
    case HostCommandKind::kShare:
    case HostCommandKind::kPrompt:
      return true;
    case HostCommandKind::kClose:
    case HostCommandKind::kGuide:
    case HostCommandKind::kCreate:
    case HostCommandKind::kDelete:
    case HostCommandKind::kActivate:
    case HostCommandKind::kUnknown:
      return false;
  }
  return false;
}

}  // namespace uagent::session
