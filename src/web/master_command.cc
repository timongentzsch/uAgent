// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "include/app/session_host.h"
#include "include/browser/browser.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
#include "include/core/thread_annotations.h"
#include "include/core/time.h"
#include "include/web/protocol.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {

namespace {
constexpr size_t kRegularCommandBytes = KiB(64);
}  // namespace

// A receipt still pending asks the host whether its worker has answered.
void Master::RefreshReceipt(Receipt& receipt, const std::string& request_id) {
  if (!receipt.worker_request.empty() &&
      JsonValue(receipt.outcome, "pending", false)) {
    receipt.outcome = host_.CommandOutcome(receipt.worker_request, request_id);
  }
}

void Master::Command(const Request& request, Response& response) {
  if (request.body.size() > kFrameBytes) {
    Error(response, "command exceeds limit", 413);
    return;
  }
  json command = json::parse(request.body, nullptr, false);
  const std::string kind = JsonValue(command, "kind", "");
  if (request.body.size() > kRegularCommandBytes && kind != "memory" &&
      kind != "skills" && kind != "instructions") {
    Error(response, "command exceeds limit", 413);
    return;
  }
  std::string request_id = JsonValue(command, "request_id", "");
  if (!command.is_object() || !OpaqueId(request_id) ||
      JsonValue(command, "v", 0) != kProtocol) {
    Error(response, "invalid command or protocol; refresh the app", 409);
    return;
  }
  UniqueLock lock(mutex_);
  std::string device = DeviceId(request);
  if (device.empty()) {
    Error(response, "device revoked", 401);
    return;
  }
  std::string key = device + ":" + request_id;
  std::string fingerprint = HashHex(request.body);
  if (auto prior = requests_.find(key); prior != requests_.end()) {
    if (prior->second.fingerprint != fingerprint) {
      Error(response, "request ID reused with different content", 409);
    } else {
      RefreshReceipt(prior->second, request_id);
      Reply(response, prior->second.outcome);
    }
    return;
  }
  // Evict only completed receipts; never forget an unacknowledged submission.
  if (request_order_.size() >= kWebRequestReceipts) {
    auto completed =
        std::find_if(request_order_.begin(), request_order_.end(),
                     [&](const std::string& id) UAGENT_REQUIRES(mutex_) {
                       const auto& receipt = requests_.at(id);
                       return !receipt.handling &&
                              !JsonValue(receipt.outcome, "pending", false);
                     });
    if (completed == request_order_.end()) {
      Error(response, "too many unacknowledged commands", 429);
      return;
    }
    requests_.erase(*completed);
    request_order_.erase(completed);
  }
  // Queue actions target a steer by its own id, never the envelope id:
  // request ids are single-use receipts and reuse is rejected above.
  std::string target = JsonValue(command, "target_id", "");
  if ((kind == "recall" || kind == "insert") && !OpaqueId(target)) {
    Error(response, "command needs a queued guidance id", 409);
    return;
  }
  // Reserve before releasing the mutex for any pipe write or process join.
  // A concurrent retry observes this same request instead of enqueueing again.
  Receipt receipt;
  receipt.fingerprint = fingerprint;
  receipt.outcome = {
      {"request_id", request_id}, {"accepted", true}, {"pending", true}};
  requests_[key] = std::move(receipt);
  request_order_.push_back(key);
  // Commands the host answers itself; any other goes to a conversation's
  // runtime. `locked` handlers change the devices.
  struct Route {
    std::string_view kind;
    bool locked;
    void (Master::*handle)(CommandCall&);
  };
  static constexpr Route kRoutes[] = {
      {"logout", true, &Master::RevokeDevice},
      {"revoke_device", true, &Master::RevokeDevice},
      {"test_notification", false, &Master::TestNotification},
      {"browser", false, &Master::BrowserCommand},
      {"memory", false, &Master::ManagementCommand},
      {"skills", false, &Master::ManagementCommand},
      {"schedule", false, &Master::ManagementCommand},
      {"models", false, &Master::ManagementCommand},
      {"permission_rules", false, &Master::ManagementCommand},
      {"instructions", false, &Master::ManagementCommand},
      {"import", false, &Master::ManagementCommand},
      {"space", false, &Master::SpaceCommand},
      {"restart_conversations", false, &Master::RestartConversations},
      {"restart_host", false, &Master::RestartHost},
      {"config", false, &Master::ConfigCommand}};
  const auto* route = std::ranges::find(kRoutes, kind, &Route::kind);
  const bool locked = route != std::end(kRoutes) && route->locked;
  auto handle =
      route != std::end(kRoutes) ? route->handle : &Master::SessionCommand;
  CommandCall call{.command = std::move(command),
                   .device = device,
                   .request_id = request_id,
                   .response = response,
                   .outcome = {{"request_id", request_id}, {"accepted", true}}};
  if (locked) {
    (this->*handle)(call);
  } else {
    lock.unlock();
    (this->*handle)(call);
    lock.lock();
    if (call.revoked && DeviceId(request) != device) call.error = call.revoked;
  }
  if (!call.worker_request.empty()) {
    requests_.at(key).worker_request = std::move(call.worker_request);
  }
  json& outcome = call.outcome;
  outcome["accepted"] = call.error.empty();
  if (!call.error.empty()) {
    outcome["error"] = call.error;
  }
  requests_.at(key).outcome = outcome;
  requests_.at(key).handling = false;
  Reply(response, outcome, call.error.empty() ? 200 : 409);
}

void Master::RevokeDevice(CommandCall& call) {
  std::string remove = JsonValue(call.command, "kind", "") == "logout"
                           ? call.device
                           : JsonValue(call.command, "device_id", "");
  auto old = devices_;
  std::erase_if(devices_,
                [&](const Device& entry) { return entry.id == remove; });
  if (!SaveDevices()) {
    devices_ = std::move(old);
    call.error = "cannot persist revocation";
    return;
  }
  push_->Revoke(remove);
  if (remove == call.device) {
    call.response.set_header(
        "Set-Cookie",
        "uagent_device=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  }
}

void Master::TestNotification(CommandCall& call) {
  std::string session_id = JsonValue(call.command, "session_id", "");
  if (!host_.Contains(session_id)) {
    call.error = "select a session before testing notifications";
  } else if (!push_->Notify(epoch_ + ":" + call.request_id, session_id,
                            call.device)) {
    call.error = "push is unavailable or queue is full";
  }
}

void Master::BrowserCommand(CommandCall& call) {
  if (browser::DataDirectory().empty()) {
    SessionCommand(call);
    return;
  }
  const std::string action = JsonValue(call.command, "action", "");
  if (action != "takeover" && action != "done" && action != "stop" &&
      action != "create_profile" && action != "select_profile" &&
      action != "setup_profile" && action != "configure") {
    call.error = "unsupported browser control";
    return;
  }
  call.outcome["result"] =
      BrowserControl(call.command, call.device, call.request_id, call.error);
  call.revoked = "device revoked";
}

void Master::ManagementCommand(CommandCall& call) {
  call.outcome["result"] = host_.Manage(call.command);
  call.error = JsonValue(call.outcome["result"], "error", "");
}

// A workspace's own: its name, and its end with everything in it.
void Master::SpaceCommand(CommandCall& call) {
  call.outcome["result"] =
      host_.SpaceControl(call.command, call.device, call.error);
}

// Settings that need a restart reach running conversations only through a
// fresh runtime; each keeps its history and restarts when idle.
void Master::RestartConversations(CommandCall& call) {
  call.outcome["result"] =
      host_.RestartRunning(JsonValue(call.command, "cwd", std::string()),
                           JsonValue(call.command, "target_id", std::string()));
}

// The stop loop lets this reply go out before it stops the server.
void Master::RestartHost(CommandCall& call) {
  reexec_ = true;
  call.outcome["result"] = {{"restarting", true}};
  WakeDescriptor(shutdown_fd);
}

void Master::ConfigCommand(CommandCall& call) {
  if (!JsonValue(call.command, "session_id", "").empty()) {
    SessionCommand(call);
    return;
  }
  json result = host_.Configure(call.command);
  PublishSettings();
  call.error = JsonValue(result, "error", "");
  call.outcome["result"] = std::move(result);
}

void Master::SessionCommand(CommandCall& call) {
  auto executed = host_.ExecuteCommand(std::move(call.command), call.device,
                                       call.request_id);
  call.outcome = std::move(executed.outcome);
  call.error = std::move(executed.error);
  call.worker_request = std::move(executed.worker_request);
  call.revoked = "device changed while handling command; refresh";
  if (executed.wake) host_wake_.Wake();
}

// Runs a browser control the viewer asked for; `done` also answers the
// conversation's pending browser decision. Called without mutex_ held.
json Master::BrowserControl(const json& command, const std::string& device,
                            const std::string& request_id, std::string& error) {
  const std::string action = JsonValue(command, "action", "");
  const std::string interaction = JsonValue(command, "interaction_id", "");
  const std::string instance = JsonValue(command, "instance_id", "0");
  json result = browser::Request(
      {{"op", action == "done" ? "prepare_done" : action},
       {"device", device},
       {"instance_id", instance},
       {"parallel_capacity", JsonValue(command, "parallel_capacity", -1)},
       {"interaction_id", interaction},
       {"name", JsonValue(command, "name", "")},
       {"profile_id", JsonValue(command, "profile_id", "")}},
      browser::kActionMs);
  error = JsonValue(result, "error", "");
  if (!error.empty() || action != "done") return result;
  const std::string session_id = JsonValue(result, "session_id", "");
  if (!interaction.empty() && session_id.empty()) {
    error = "browser conversation is missing";
    return result;
  }
  if (!interaction.empty()) {
    json reply = {{"kind", "reply"},
                  {"interaction_id", interaction},
                  {"text", "done"},
                  {"request_id", request_id}};
    session::StampFrame(reply, session_id,
                        JsonValue(command, "generation", ""));
    auto executed = host_.ExecuteCommand(reply, device, request_id);
    error = executed.error;
    if (!error.empty()) return result;
    if (executed.wake) host_wake_.Wake();
  }
  result = browser::Request({{"op", "done"},
                             {"device", device},
                             {"instance_id", instance},
                             {"interaction_id", interaction}});
  error = JsonValue(result, "error", "");
  if (error == "browser service timed out or disconnected") {
    json current = browser::Request(
        {{"op", "status"}, {"instance_id", instance}}, browser::kProbeMs);
    if (current.value("ok", false) &&
        JsonValue(current, "mode", "") != "human" &&
        JsonValue(current, "interaction_id", "").empty() &&
        JsonValue(current, "session_id", "") == session_id) {
      result = std::move(current);
      error.clear();
    }
  }
  return result;
}
}  // namespace uagent::web::master
