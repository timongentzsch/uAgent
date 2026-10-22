// Copyright 2026 Timon Gentzsch

#include "include/app/outcome_store.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "include/app/session_host.h"
#include "include/core/json.h"
#include "include/core/thread_annotations.h"

namespace uagent::session {
namespace {

// Unacknowledged worker commands held at once; beyond this the oldest
// completed receipt is evicted, and a log with nothing completable refuses.
constexpr size_t kMaxOutcomes = 512;
// How long SendCommand waits for the worker's outcome before returning the
// pending receipt; the late outcome still resolves it via ResolveOutcome.
constexpr std::chrono::seconds kOutcomeWait{2};

}  // namespace

json OutcomeStore::SendCommand(const std::shared_ptr<HostSession>& session,
                               json command, const std::string& worker_request,
                               const std::string& client_request,
                               bool& dispatched) {
  dispatched = false;
  std::shared_ptr<json> receipt;
  {
    std::lock_guard lock(mutex_);
    if (auto found = outcomes_.find(worker_request); found != outcomes_.end()) {
      receipt = found->second;
    } else {
      if (order_.size() >= kMaxOutcomes) {
        auto completed = std::find_if(
            order_.begin(), order_.end(),
            [&](const std::string& id) UAGENT_REQUIRES(mutex_) {
              return !JsonValue(*outcomes_.at(id), "pending", false);
            });
        if (completed == order_.end()) {
          return {{"request_id", client_request},
                  {"accepted", false},
                  {"error", "too many unacknowledged worker commands"}};
        }
        outcomes_.erase(*completed);
        order_.erase(completed);
      }
      receipt = std::make_shared<json>();
      outcomes_[worker_request] = receipt;
      order_.push_back(worker_request);
    }
    *receipt = {
        {"request_id", client_request}, {"accepted", true}, {"pending", true}};
    session->awaiting.push_back(worker_request);
  }
  command["request_id"] = worker_request;
  if (!session->Send(std::move(command))) {
    std::lock_guard lock(mutex_);
    std::erase(session->awaiting, worker_request);
    return *receipt = {{"request_id", client_request},
                       {"accepted", false},
                       {"error", "worker is disconnected"}};
  }
  dispatched = true;
  UniqueLock lock(mutex_);
  changed_.wait_for(lock, kOutcomeWait, [&]() UAGENT_REQUIRES(mutex_) {
    return !JsonValue(*receipt, "pending", false) || session->exited;
  });
  return *receipt;
}

json OutcomeStore::CommandOutcome(const std::string& worker_request,
                                  const std::string& client_request) const {
  std::lock_guard lock(mutex_);
  auto found = outcomes_.find(worker_request);
  return found == outcomes_.end()
             ? json{{"request_id", client_request}, {"unknown", true}}
             : *found->second;
}

size_t OutcomeStore::PendingCommands(const HostSession& session) const {
  std::lock_guard lock(mutex_);
  return session.awaiting.size();
}

bool OutcomeStore::ResolveOutcome(HostSession& session, json& frame) {
  const std::string worker_request = JsonValue(frame, "request_id", "");
  std::lock_guard lock(mutex_);
  auto found = outcomes_.find(worker_request);
  if (found == outcomes_.end()) return false;
  const std::string client_request =
      JsonValue(*found->second, "request_id", "");
  frame["request_id"] = client_request;
  *found->second = {{"request_id", client_request},
                    {"accepted", JsonValue(frame, "accepted", false)},
                    {"pending", JsonValue(frame, "pending", false)},
                    {"error", JsonValue(frame, "error", "")},
                    {"result", JsonValue(frame, "result", json::object())}};
  if (!JsonValue(frame, "pending", false)) {
    std::erase(session.awaiting, worker_request);
  }
  changed_.notify_all();
  return true;
}

void OutcomeStore::FailPending(HostSession& session) {
  std::lock_guard lock(mutex_);
  for (const std::string& worker_request : session.awaiting) {
    auto found = outcomes_.find(worker_request);
    if (found == outcomes_.end()) continue;
    *found->second = {
        {"request_id", JsonValue(*found->second, "request_id", "")},
        {"accepted", false},
        {"error",
         "worker closed before acknowledgement; inspect history "
         "before retrying"}};
  }
  session.awaiting.clear();
  changed_.notify_all();
}

}  // namespace uagent::session
