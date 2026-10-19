// Copyright 2026 Timon Gentzsch
#include "include/browser/broker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "include/browser/runtime.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/private_store.h"
#include "include/core/thread_annotations.h"
#include "include/tools/files.h"
#include "include/transport/session.h"

namespace uagent::browser {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int kMaxResearch = 4;
constexpr size_t kMaxWaiting = 64;
constexpr auto kWaitHeartbeat = std::chrono::seconds(15);
constexpr auto kOwnerQuiet = std::chrono::minutes(3);
std::string LockPath(const std::string& session) {
  const char* named = getenv("UAGENT_INTERNAL_RUNTIME_DIR");
  return session::SocketPathForId(session,
                                  named && *named ? named : RuntimeDir()) +
         ".lock";
}
bool Live(const std::string& session) {
  return FileLease::HasLiveOwner(LockPath(session));
}
}  // namespace

struct Broker::State {
  struct Slot {
    explicit Slot(std::string base)
        : directory(std::move(base)),
          runtime(std::make_unique<Runtime>(directory)) {
      status = runtime->Execute({{"op", "snapshot"}});
      owner = JsonValue(status, "session_id", "");
      if (!owner.empty()) {
        token = runtime_token = RandomToken(16);
        live = Live(owner);
      }
    }
    std::mutex mutex;
    std::string directory, runtime_token;
    std::unique_ptr<Runtime> runtime;
    // Everything below is protected by State::mutex. The slot mutex is held
    // first whenever both are needed; scheduling never takes a slot mutex.
    json status = {{"ok", true}, {"mode", "idle"}, {"running", false}};
    std::string owner, token, previous;
    Clock::time_point used = Clock::now();
    bool live = false, busy = false, release_pending = false;
  };
  struct Waiter {
    std::string session, request;
    bool shared;
    Clock::time_point polled;
  };
  explicit State(std::string base, Now clock)
      : directory(std::move(base)), now(std::move(clock)) {
    slots[0] = std::make_unique<Slot>(directory);
    for (int i = 1; i <= kMaxResearch; ++i) {
      slots[static_cast<size_t>(i)] =
          std::make_unique<Slot>(directory + "/p" + std::to_string(i));
    }
    const json saved = ReadJsonFile(directory + "/scheduler.json", 4096);
    capacity =
        std::clamp(JsonValue(saved, "parallel_capacity", 0), 0, kMaxResearch);
  }
  void Clear(Slot& slot) UAGENT_REQUIRES(mutex) {
    slot.previous = std::exchange(slot.owner, "");
    slot.token.clear();
    slot.release_pending = false;
    slot.status["session_id"] = "";
  }
  void Prune() UAGENT_REQUIRES(mutex) {
    const auto current = now();
    std::erase_if(waiting, [&](const Waiter& w) {
      return current - w.polled > kWaitHeartbeat;
    });
    for (auto& ptr : slots) {
      Slot& slot = *ptr;
      if (slot.owner.empty() || slot.busy ||
          JsonValue(slot.status, "mode", "") == "human")
        continue;
      if ((slot.live && !Live(slot.owner)) ||
          current - slot.used >= kOwnerQuiet)
        Clear(slot);
    }
  }
  int Owned(const std::string& session) const UAGENT_REQUIRES(mutex) {
    if (!session.empty()) {
      for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i]->owner == session) return static_cast<int>(i);
      }
    }
    return -1;
  }
  int Instance(const json& command) const {
    const std::string id = JsonValue(command, "instance_id", "0");
    if (id.size() != 1 || id[0] < '0' || id[0] > '0' + kMaxResearch) return -1;
    return id[0] - '0';
  }
  json Summary(int index) const UAGENT_REQUIRES(mutex) {
    const Slot& slot = *slots[static_cast<size_t>(index)];
    json result = slot.status;
    result["instance_id"] = std::to_string(index);
    result["session_id"] = slot.owner;
    result["isolated"] = index != 0;
    return result;
  }
  json Status(int index) const UAGENT_REQUIRES(mutex) {
    json result = Summary(index);
    result["parallel_capacity"] = capacity;
    result["instances"] = json::array();
    for (int i = 0; i <= kMaxResearch; ++i) {
      if (i <= capacity || !slots[static_cast<size_t>(i)]->owner.empty() ||
          JsonValue(slots[static_cast<size_t>(i)]->status, "mode", "") ==
              "human") {
        json instance = Summary(i);
        instance.erase("viewer");
        result["instances"].push_back(std::move(instance));
      }
    }
    result["queue"] = json::array();
    for (const auto& w : waiting) {
      result["queue"].push_back(
          {{"session_id", w.session}, {"shared", w.shared}});
    }
    return result;
  }
  std::mutex mutex;
  std::string directory;
  std::array<std::unique_ptr<Slot>, kMaxResearch + 1> slots;
  std::deque<Waiter> waiting UAGENT_GUARDED_BY(mutex);
  Now now;
  int capacity UAGENT_GUARDED_BY(mutex) = 0;
  bool stopping UAGENT_GUARDED_BY(mutex) = false;
};

Broker::Broker(std::string directory, Now now)
    : state_(std::make_unique<State>(std::move(directory), std::move(now))) {}
Broker::~Broker() { Shutdown(); }

json Broker::Execute(const json& command) {
  State& s = *state_;
  const std::string op = JsonValue(command, "op", "");
  const std::string session = JsonValue(command, "session_id", "");
  int index = -1;
  {
    std::lock_guard lock(s.mutex);
    if (s.stopping) return {{"error", "browser service is stopping"}};
    if (op == "ping") return {{"ok", true}};
    s.Prune();
    index = s.Owned(session);
    if (op == "configure") {
      int capacity = JsonValue(command, "parallel_capacity", -1);
      if (capacity < 0 || capacity > kMaxResearch) {
        return {{"error", "parallel browser capacity must be between 0 and 4"}};
      }
      if (!ToolWritePrivateFile(s.directory + "/scheduler.json",
                                JsonDump({{"parallel_capacity", capacity}}))
               .Ok()) {
        return {{"error", "cannot save browser scheduling settings"}};
      }
      s.capacity = capacity;
      if (capacity == 0)
        for (auto& waiter : s.waiting) waiter.shared = true;
      return s.Status(s.Instance(command) < 0 ? 0 : s.Instance(command));
    }
    if (op == "acquire" || op == "cancel_acquire") {
      const std::string request = JsonValue(command, "request_id", "");
      if (!session::OpaqueId(session) || !session::OpaqueId(request)) {
        return {{"error", "invalid browser acquisition"}};
      }
      if (op == "cancel_acquire") {
        std::erase_if(s.waiting, [&](const State::Waiter& w) {
          return w.session == session && w.request == request;
        });
        return {{"ok", true}};
      }
      if (index >= 0) {
        auto& slot = *s.slots[static_cast<size_t>(index)];
        slot.used = s.now();
        if (JsonValue(slot.status, "mode", "") == "human" &&
            !JsonValue(command, "allow_human", false)) {
          return {{"ok", true},
                  {"acquired", false},
                  {"position", 0},
                  {"reason", "Waiting for browser handback"}};
        }
        return {{"ok", true},
                {"acquired", true},
                {"lease", slot.token},
                {"instance_id", std::to_string(index)}};
      }
      auto found = std::find_if(
          s.waiting.begin(), s.waiting.end(), [&](const State::Waiter& w) {
            return w.session == session && w.request == request;
          });
      if (found == s.waiting.end()) {
        if (s.waiting.size() >= kMaxWaiting) {
          return {{"error", "browser waiting queue is full"}};
        }
        s.waiting.push_back(
            {session, request,
             JsonValue(command, "browser", "auto") == "shared" ||
                 s.capacity == 0,
             s.now()});
        found = std::prev(s.waiting.end());
      }
      found->polled = s.now();
      size_t position = 1;
      bool first = true;
      for (auto at = s.waiting.begin(); at != found; ++at) {
        if (at->shared == found->shared) {
          first = false;
          ++position;
        }
      }
      if (first) {
        for (int i = found->shared ? 0 : 1;
             i <= (found->shared ? 0 : s.capacity); ++i) {
          auto& slot = *s.slots[static_cast<size_t>(i)];
          if (!slot.owner.empty() || slot.busy ||
              JsonValue(slot.status, "mode", "") == "human")
            continue;
          slot.token = RandomToken(16);
          if (slot.token.empty())
            return {{"error", "cannot create browser lease"}};
          slot.owner = session;
          slot.live = Live(session);
          slot.used = s.now();
          std::erase_if(s.waiting, [&](const State::Waiter& w) {
            return w.session == session;
          });
          return {{"ok", true},
                  {"acquired", true},
                  {"lease", slot.token},
                  {"instance_id", std::to_string(i)}};
        }
      }
      return {{"ok", true},
              {"acquired", false},
              {"position", position},
              {"reason", "Waiting for browser"}};
    }
    if (op == "release") {
      std::erase_if(s.waiting, [&](const State::Waiter& w) {
        return w.session == session;
      });
      if (index < 0) return {{"ok", true}};
    }
    if (op == "status" || op == "agent_status" || op == "viewer") {
      if (index < 0) index = s.Instance(command);
      if (index < 0) return {{"error", "unknown browser instance"}};
      if (op == "viewer") {
        // Viewer heartbeats must not occupy service threads behind a slow
        // page operation. During a control transition, gate input closed
        // until the new snapshot has been published.
        const auto& slot = *s.slots[static_cast<size_t>(index)];
        json result = s.Summary(index);
        if (!JsonValue(result, "running", false))
          return {{"error", "the browser is not running"}};
        result["controller"] =
            !slot.busy && JsonValue(result, "mode", "") == "human" &&
            JsonValue(result, "viewer", "") == JsonValue(command, "device", "");
        return result;
      }
      return s.Status(index);
    }
    if (index < 0 && !session.empty())
      return {{"error", "acquire the browser before using it"}};
    if (index < 0) index = s.Instance(command);
    if (index < 0) return {{"error", "unknown browser instance"}};
  }
  auto& slot = *s.slots[static_cast<size_t>(index)];
  std::lock_guard runtime_lock(slot.mutex);
  std::string previous, token;
  uint64_t incarnation = 0;
  {
    std::lock_guard lock(s.mutex);
    if (s.stopping) return {{"error", "browser service is stopping"}};
    if (!session.empty() && (slot.owner != session ||
                             ((op != "release" || command.contains("lease")) &&
                              JsonValue(command, "lease", "") != slot.token))) {
      return {{"error", "browser lease changed; acquire it again and observe"}};
    }
    slot.busy = true;
    token = slot.token;
    incarnation = std::max(JsonValue(slot.status, "display", uint64_t{0}),
                           JsonValue(slot.status, "generation", uint64_t{0})) +
                  1;
    previous = std::exchange(slot.previous, "");
    if (index > 0 && slot.runtime_token != token) {
      // A research grant gets a fresh profile, never the previous thread's
      // cookies, tabs or login state. Shutdown runs outside the broker lock.
      previous.clear();
    }
  }
  if (index > 0 && slot.runtime_token != token) {
    slot.runtime->Shutdown();
    std::error_code error;
    std::filesystem::remove_all(slot.directory, error);
    if (error) {
      std::lock_guard lock(s.mutex);
      slot.busy = false;
      return {{"error", "cannot reset isolated browser profile"}};
    }
    slot.runtime = std::make_unique<Runtime>(slot.directory, incarnation);
    slot.runtime_token = token;
  }
  if (!previous.empty())
    slot.runtime->Execute({{"op", "release"}, {"session_id", previous}});
  json result = slot.runtime->Execute(command);
  json snapshot =
      slot.runtime->Execute({{"op", op == "viewer" ? "snapshot" : "status"}});
  {
    std::lock_guard lock(s.mutex);
    if (op == "release" && JsonValue(snapshot, "mode", "") == "human") {
      slot.release_pending = true;
    }
  }
  bool release = false;
  {
    std::lock_guard lock(s.mutex);
    release =
        slot.release_pending && JsonValue(snapshot, "mode", "") != "human";
  }
  if (release) {
    slot.runtime->Execute(
        {{"op", "release"},
         {"session_id", JsonValue(snapshot, "session_id", "")}});
    snapshot = slot.runtime->Execute({{"op", "snapshot"}});
  }
  {
    std::lock_guard lock(s.mutex);
    slot.status = std::move(snapshot);
    slot.busy = false;
    slot.used = s.now();
    if (release ||
        (op == "release" && JsonValue(slot.status, "mode", "") != "human") ||
        (op == "stop" && !result.contains("error"))) {
      if (op == "release")
        result = {{"ok", true}, {"instance_id", std::to_string(index)}};
      s.Clear(slot);
    }
    result["instance_id"] = std::to_string(index);
  }
  return result;
}

void Broker::Maintain() {
  for (auto& slot : state_->slots) {
    std::unique_lock lock(slot->mutex, std::try_to_lock);
    if (!lock) continue;
    slot->runtime->StopIfIdle(std::chrono::minutes(kIdleMinutes));
    json snapshot = slot->runtime->Execute({{"op", "snapshot"}});
    std::lock_guard state_lock(state_->mutex);
    slot->status = std::move(snapshot);
  }
}
void Broker::Shutdown() {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->waiting.clear();
  }
  for (auto& slot : state_->slots) {
    std::lock_guard lock(slot->mutex);
    slot->runtime->Shutdown();
  }
}
}  // namespace uagent::browser
