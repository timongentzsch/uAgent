// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/session_host.h"
#include "include/browser/browser.h"
#include "include/core/effective_config.h"
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
  // Recall targets a queued steer by its own id, never the envelope id:
  // request ids are single-use receipts and reuse is rejected above.
  std::string target = JsonValue(command, "target_id", "");
  if (kind == "recall" && !OpaqueId(target)) {
    Error(response, "recall needs a queued guidance id", 409);
    return;
  }
  json outcome = {{"request_id", request_id}, {"accepted", true}};
  // Reserve before releasing the mutex for any pipe write or process join.
  // A concurrent retry observes this same request instead of enqueueing again.
  Receipt receipt;
  receipt.fingerprint = fingerprint;
  receipt.outcome = {
      {"request_id", request_id}, {"accepted", true}, {"pending", true}};
  requests_[key] = std::move(receipt);
  request_order_.push_back(key);
  std::string error;
  if (kind == "logout" || kind == "revoke_device") {
    std::string remove =
        kind == "logout" ? device : JsonValue(command, "device_id", "");
    auto old = devices_;
    std::erase_if(devices_,
                  [&](const Device& entry) { return entry.id == remove; });
    if (!SaveDevices()) {
      devices_ = std::move(old);
      error = "cannot persist revocation";
    } else {
      push_->Revoke(remove);
      if (remove == device) {
        response.set_header(
            "Set-Cookie",
            "uagent_device=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
      }
    }
  } else if (kind == "test_notification") {
    std::string session_id = JsonValue(command, "session_id", "");
    if (!host_.Contains(session_id)) {
      error = "select a session before testing notifications";
    } else if (!push_->Notify(epoch_ + ":" + request_id, session_id, device)) {
      error = "push is unavailable or queue is full";
    }
  } else if (kind == "browser" && !browser::DataDirectory().empty()) {
    const std::string action = JsonValue(command, "action", "");
    if (action != "takeover" && action != "done" && action != "stop" &&
        action != "create_profile" && action != "select_profile" &&
        action != "setup_profile") {
      error = "unsupported browser control";
    } else {
      lock.unlock();
      std::string control_error;
      json result = BrowserControl(command, device, request_id, control_error);
      lock.lock();
      if (DeviceId(request) != device) control_error = "device revoked";
      error = std::move(control_error);
      outcome["result"] = result;
    }
  } else if (kind == "memory" || kind == "skills" || kind == "schedule" ||
             kind == "models" || kind == "permission_rules" ||
             kind == "tool_categories" || kind == "instructions" ||
             kind == "import") {
    lock.unlock();
    auto result = ControlProcess(command);
    lock.lock();
    outcome["result"] = result;
    error = JsonValue(result, "error", "");
    auto action = JsonValue(command, "action", "list");
    if (error.empty() && action != "list" && action != "get" &&
        action != "show" && action != "preview") {
      host_.Publish("", "", {{"kind", "management.changed"}});
    }
  } else if (kind == "space") {
    // A workspace's own: its name, and its end with everything in it.
    lock.unlock();
    const std::string path = JsonValue(command, "cwd", "");
    error = JsonValue(command, "action", "") == "delete"
                ? host_.DeleteSpace(path, device)
                : host_.RenameSpace(path, Trim(JsonValue(command, "name", "")));
    lock.lock();
    outcome["result"] = {{"spaces", session::Spaces()}};
    if (error.empty()) {
      host_.Publish(
          "", "", {{"kind", "spaces.changed"}, {"spaces", session::Spaces()}});
    }
  } else if (kind == "restart_conversations") {
    // Settings that need a restart reach running conversations only through
    // a fresh runtime; each keeps its history and restarts when idle.
    lock.unlock();
    outcome["result"] =
        host_.RestartRunning(JsonValue(command, "cwd", std::string()),
                             JsonValue(command, "target_id", std::string()));
    lock.lock();
  } else if (kind == "restart_host") {
    // The stop loop lets this reply go out before it stops the server.
    reexec_ = true;
    outcome["result"] = {{"restarting", true}};
    WakeDescriptor(shutdown_fd);
  } else if (kind == "config" && JsonValue(command, "session_id", "").empty()) {
    lock.unlock();
    // The host belongs to no project: a request names the folder it is
    // about, and without one only what is saved for all is in reach.
    const auto folder = CanonicalDirectory(JsonValue(command, "cwd", ""));
    auto manager =
        ConfigManager::Capture({}, folder ? folder->string() : std::string());
    auto result = ConfigurationControl(command, manager);
    PublishSettings();
    lock.lock();
    outcome["result"] = result;
    error = JsonValue(result, "error", "");
  } else {
    lock.unlock();
    auto executed =
        host_.ExecuteCommand(std::move(command), device, request_id);
    lock.lock();
    if (DeviceId(request) != device) {
      executed.error = "device changed while handling command; refresh";
    }
    outcome = std::move(executed.outcome);
    error = std::move(executed.error);
    if (!executed.worker_request.empty()) {
      requests_.at(key).worker_request = std::move(executed.worker_request);
    }
    if (executed.wake) host_wake_.Wake();
  }
  outcome["accepted"] = error.empty();
  if (!error.empty()) {
    outcome["error"] = error;
  }
  requests_.at(key).outcome = outcome;
  requests_.at(key).handling = false;
  Reply(response, outcome, error.empty() ? 200 : 409);
}

// Runs a browser control the viewer asked for; `done` also answers the
// conversation's pending browser decision. Called without mutex_ held.
json Master::BrowserControl(const json& command, const std::string& device,
                            const std::string& request_id, std::string& error) {
  const std::string action = JsonValue(command, "action", "");
  const std::string interaction = JsonValue(command, "interaction_id", "");
  json result =
      browser::Request({{"op", action == "done" ? "prepare_done" : action},
                        {"device", device},
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
  result = browser::Request(
      {{"op", "done"}, {"device", device}, {"interaction_id", interaction}});
  error = JsonValue(result, "error", "");
  if (error == "browser service timed out or disconnected") {
    json current = browser::Request({{"op", "status"}}, browser::kProbeMs);
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
