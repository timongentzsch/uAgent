// Copyright 2026 Timon Gentzsch

#include "include/tools/collaborator_runtime.h"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/jobs.h"
#include "include/agent/session_store.h"
#include "include/app/session.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

using Clock = std::chrono::steady_clock;

bool NextFrame(int fd, std::string& input, Clock::time_point deadline,
               json& frame, bool interruptible) {
  for (;;) {
    const size_t newline = input.find('\n');
    if (newline != std::string::npos) {
      if (newline > session::kFrameBytes) return false;
      frame = json::parse(input.substr(0, newline), nullptr, false);
      input.erase(0, newline + 1);
      return frame.is_object() &&
             JsonValue(frame, "v", 0) == session::kProtocol;
    }
    if (input.size() > session::kFrameBytes || Clock::now() >= deadline) {
      return false;
    }
    if (interruptible && AbortRequested()) return false;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    pollfd ready{fd, POLLIN, 0};
    const int wait = static_cast<int>(
        std::clamp<int64_t>(remaining.count(), int64_t{1}, int64_t{100}));
    const int found = poll(&ready, 1, wait);
    if (found < 0 && errno == EINTR) continue;
    if (found < 0 || (ready.revents & (POLLERR | POLLNVAL))) {
      return false;
    }
    if (!(ready.revents & (POLLIN | POLLHUP))) continue;
    char buffer[8192];
    const ssize_t count = read(fd, buffer, sizeof buffer);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count <= 0) return false;
    input.append(buffer, static_cast<size_t>(count));
  }
}

json Command(const std::string& path, const std::string& generation,
             session::Connection& connection, std::string kind,
             std::string text, Clock::time_point deadline, bool checkpoint,
             bool* submitted = nullptr, const json& budget = json::object(),
             bool observe_busy = false) {
  const std::string request = session::RandomToken(16);
  json command = {{"v", session::kProtocol},  {"session_id", HashHex(path)},
                  {"generation", generation}, {"kind", std::move(kind)},
                  {"request_id", request},    {"client_request_id", request}};
  if (!text.empty()) command["text"] = std::move(text);
  if (!budget.empty()) command["budget"] = budget;
  if (!session::WriteFrame(connection.socket.Get(), command)) {
    return {{"command_error", "collaborator connection closed"},
            {"uncertain", true}};
  }
  if (submitted) *submitted = true;
  std::string input;
  json latest = json::object();
  bool accepted = false;
  const bool wait_idle =
      JsonValue(command, "kind", "") == "refresh" && !observe_busy;
  for (;;) {
    json frame;
    if (!NextFrame(connection.socket.Get(), input, deadline, frame,
                   checkpoint)) {
      return {{"command_error", "collaborator did not reach a checkpoint"},
              {"uncertain", true}};
    }
    const std::string frame_kind = JsonValue(frame, "kind", "");
    if (frame_kind == "outcome" &&
        JsonValue(frame, "request_id", "") == request) {
      if (!JsonValue(frame, "accepted", false)) {
        std::string reason = JsonValue(frame, "error", "");
        return {
            {"command_error", reason.empty() ? "command rejected" : reason}};
      }
      if (JsonValue(frame, "pending", false)) continue;
      accepted = true;
      if (!checkpoint && (!wait_idle || !JsonValue(latest, "busy", true))) {
        return latest;
      }
    }
    if (frame_kind != "state") continue;
    latest = JsonValue(frame, "state", json::object());
    latest["busy"] = JsonValue(frame, "busy", false);
    if ((!checkpoint && accepted && !JsonValue(frame, "busy", true)) ||
        (checkpoint && JsonValue(frame, "checkpoint", false) &&
         JsonValue(frame, "completed_request_id", "") == request)) {
      return latest;
    }
  }
}

std::string LastAnswer(const std::string& path) {
  auto loaded = SessionStore::Inspect(path);
  if (!loaded.record) return {};
  Conversation conversation;
  const auto& state = loaded.record->state;
  if (!conversation.Restore(state.messages, state.message_kinds, state.archive,
                            state.archive_dropped_segments, state.tool_displays,
                            state.display)) {
    return {};
  }
  return conversation.LastAssistantText();
}

std::string CheckpointAnswer(const json& state, const std::string& path) {
  if (const json* view = JsonObject(state, "view")) {
    if (const json* blocks = JsonArray(*view, "blocks")) {
      for (auto block = blocks->rbegin(); block != blocks->rend(); ++block) {
        if (JsonValue(*block, "kind", "") == "assistant") {
          std::string text = JsonValue(*block, "text", "");
          if (!text.empty()) return text;
        }
      }
    }
  }
  return LastAnswer(path);
}

json WaitIdle(const std::string& path, const std::string& generation,
              Clock::time_point deadline) {
  auto connection = session::Connect(path);
  if (!connection.socket || connection.generation != generation) {
    return json::object();
  }
  json state = Command(path, connection.generation, connection, "refresh", "",
                       deadline, false);
  return state.contains("command_error") ? json::object() : state;
}

void PublishCollaborator(json collaborator, bool removed = false) {
  Emit(
      Event{EventId::kCollaboratorChanged,
            {{"collaborator", std::move(collaborator)}, {"removed", removed}}});
}

}  // namespace

struct CollaboratorRuntime::State {
  struct Slot {
    std::string path, generation, label, model, route, handoff, identity;
    Fd owner;
    pid_t pid = -1;
    uint64_t handoff_generation = 0;
    bool active = false;
    Usage accounted;
    RouteUsage accounted_routes;
    json accounted_statistics = json::object();
    int64_t parent_turn = 0;
    json latest = json::object();
  };

  explicit State(UsageAccumulator& target) : usage(target) {}

  void Account(Slot& slot, const json& snapshot) {
    const Usage current = UsageFromJson(JsonValue(snapshot, "usage", json{}));
    const RouteUsage current_routes =
        RouteUsageFromJson(JsonValue(snapshot, "route_usage", json{}));
    const json current_statistics =
        JsonValue(snapshot, "statistics", json::object());
    const json statistics = FlattenNumericStatistics(
        NumericStatisticsDifference(current_statistics,
                                    slot.accounted_statistics),
        "side_");
    if (current_routes.empty()) {
      usage.Add(slot.route.empty() ? "delegated/unknown" : slot.route,
                UsageDifference(current, slot.accounted), slot.parent_turn,
                statistics);
    } else {
      bool first = true;
      for (const auto& [route_name, value] : current_routes) {
        auto prior = slot.accounted_routes.find(route_name);
        usage.Add(route_name,
                  UsageDifference(value, prior == slot.accounted_routes.end()
                                             ? Usage{}
                                             : prior->second),
                  slot.parent_turn, first ? statistics : json::object());
        first = false;
      }
    }
    slot.accounted = current;
    slot.accounted_routes = current_routes;
    slot.accounted_statistics = current_statistics;
  }

  UsageAccumulator& usage;
  mutable std::mutex mutex;
  std::map<std::string, Slot> slots;
  // Handoff counts survive Stop so a resumed-after-loss record still shows
  // how many briefs this collaborator has received.
  std::map<std::string, uint64_t> generations;
  bool shutdown = false;
};

CollaboratorRuntime::CollaboratorRuntime(UsageAccumulator& usage)
    : state_(std::make_unique<State>(usage)) {}

CollaboratorRuntime::~CollaboratorRuntime() { Shutdown(); }

ToolResult CollaboratorRuntime::Handoff(CollaboratorLaunch launch,
                                        const std::string& prompt,
                                        const ToolContext& context,
                                        bool* submitted) {
  if (submitted) *submitted = false;
  State& state = *state_;
  session::Connection connection;
  std::string request_generation;
  std::string id, path;
  bool runtime_lost = false;
  {
    std::lock_guard lock(state.mutex);
    if (state.shutdown) {
      return ToolFailure(ToolErrorCode::kUnavailable,
                         "error: collaborator runtime is stopping");
    }
    auto existing = state.slots.find(launch.id);
    if (existing != state.slots.end() && existing->second.active) {
      return ToolFailure(ToolErrorCode::kUnavailable,
                         "error: collaborator " + launch.id +
                             " already has an active handoff");
    }
    if (existing == state.slots.end() &&
        state.slots.size() >= static_cast<size_t>(PersistentMax())) {
      return ToolFailure(
          ToolErrorCode::kLimitExceeded,
          "error: persistent limit reached (" +
              std::to_string(PersistentMax()) +
              "); stop a retained collaborator to free its runtime");
    }
    connection = session::Connect(launch.path);
    runtime_lost = existing != state.slots.end() &&
                   (!connection.socket ||
                    connection.generation != existing->second.generation);
    if (existing != state.slots.end()) {
      id = existing->first;
      path = existing->second.path;
      if (runtime_lost) existing->second.active = false;
    }
    if (!connection.socket && !runtime_lost) {
      std::string error;
      connection = session::Open(ExecutablePath(), launch.cwd, launch.path,
                                 launch.title, launch.options, error);
      if (!connection.socket) {
        return ToolFailure(ToolErrorCode::kProcessFailed,
                           "error: " + std::move(error));
      }
    }
    if (!runtime_lost) {
      State::Slot& slot = state.slots[launch.id];
      if (slot.path.empty()) {
        if (auto loaded = SessionStore::Inspect(launch.path); loaded.record) {
          slot.accounted = loaded.record->state.usage;
          slot.accounted_routes = loaded.record->state.route_usage;
          slot.accounted_statistics = JsonValue(loaded.record->state.display,
                                                "statistics", json::object());
        }
      }
      id = launch.id;
      slot.path = std::move(launch.path);
      slot.generation = connection.generation;
      if (connection.owner) slot.owner = std::move(connection.owner);
      slot.pid = connection.pid;
      slot.identity = ProcessIdentity(connection.pid);
      slot.label = std::move(launch.title);
      slot.model = std::move(launch.model);
      slot.route = std::move(launch.route);
      slot.parent_turn = launch.parent_turn;
      slot.active = true;
      request_generation = session::RandomToken(16);
      slot.handoff = request_generation;
      slot.handoff_generation = ++state.generations[id];
      path = slot.path;
    }
  }
  if (runtime_lost) {
    // The new brief was never submitted. Tear down the stale identity so a
    // later explicit follow-up can resume the saved conversation knowingly.
    (void)Stop(id);
    return ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: persistent collaborator runtime was lost; live handles are "
        "invalid, inspect the workspace before an explicit follow-up");
  }
  PublishCollaborator(Snapshot(id));

  auto deadline = context.deadline;
  if (deadline == Clock::time_point::max()) {
    deadline = Clock::now() + std::chrono::hours(24);
  }
  json result = Command(
      path, connection.generation, connection, "submit", prompt, deadline, true,
      submitted,
      {{"cost", launch.remaining_cost}, {"tokens", launch.remaining_tokens}});
  if (result.contains("command_error")) {
    // A live socket is not proof that a lost/rejected handoff is idle.
    // Confirm quiescence before the parent may resume editing.
    Interrupt(id);
    const json idle = WaitIdle(path, connection.generation,
                               Clock::now() + std::chrono::seconds(5));
    const bool retained = !idle.empty();
    if (retained) {
      std::lock_guard lock(state.mutex);
      if (auto it = state.slots.find(id); it != state.slots.end()) {
        state.Account(it->second, idle);
      }
    }
    if (!retained) Stop(id);
    if (retained) {
      std::lock_guard lock(state.mutex);
      if (auto it = state.slots.find(id);
          it != state.slots.end() && it->second.handoff == request_generation) {
        it->second.active = false;
      }
    }
    if (retained) PublishCollaborator(Snapshot(id));
    if (context.Expired()) {
      return ToolTimedOut(
          retained ? "error: persistent collaborator handoff timed out; the "
                     "runtime was interrupted and retained"
                   : "error: persistent collaborator handoff timed out; the "
                     "runtime was stopped because quiescence could not be "
                     "confirmed");
    }
    if (AbortRequested()) {
      return ToolCancelled(
          retained ? "error: persistent collaborator handoff interrupted; the "
                     "runtime was retained"
                   : "error: persistent collaborator handoff interrupted; the "
                     "runtime was stopped because quiescence could not be "
                     "confirmed");
    }
    return ToolFailure(
        ToolErrorCode::kProcessFailed, "error: " + [&] {
          std::string reason = JsonValue(result, "command_error", "");
          if (reason.empty()) reason = "handoff failed";
          if (!retained) reason += "; collaborator runtime was stopped";
          return reason;
        }());
  }
  {
    std::lock_guard lock(state.mutex);
    auto it = state.slots.find(id);
    if (it == state.slots.end() || it->second.handoff != request_generation) {
      return ToolCancelled("error: collaborator handoff was superseded");
    }
    it->second.active = false;
    it->second.latest = result;
    state.Account(it->second, result);
  }
  PublishCollaborator(Snapshot(id));
  const std::string error = JsonValue(result, "error", "");
  const std::string answer = CheckpointAnswer(result, path);
  const std::string reason =
      JsonValue(JsonValue(result, "stop", json::object()), "reason", "");
  if (reason == "cancelled") {
    return ToolCancelled("error: persistent collaborator handoff interrupted");
  }
  if (reason == "turn_deadline") {
    return ToolTimedOut("error: persistent collaborator handoff timed out");
  }
  if (!reason.empty() && reason != "completed" && error.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: persistent collaborator stopped: " + reason);
  }
  if (!error.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: persistent collaborator handoff failed: " +
                           TerminalSafe(error));
  }
  if (answer.empty()) {
    return ToolFailure(
        ToolErrorCode::kRemoteError,
        "error: persistent collaborator produced no answer; "
        "checkpoint=" +
            Utf8Trunc(JsonDump(JsonValue(result, "view", json::object())),
                      1024));
  }
  return ToolSuccess(answer);
}

ToolResult CollaboratorRuntime::Control(const std::string& id,
                                        const std::string& kind,
                                        const std::string& text) {
  State& state = *state_;
  std::string path, generation;
  {
    std::lock_guard lock(state.mutex);
    auto it = state.slots.find(id);
    if (it == state.slots.end() || it->second.path.empty()) {
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: persistent collaborator is not live");
    }
    path = it->second.path;
    generation = it->second.generation;
  }
  auto connection = session::Connect(path);
  if (!connection.socket || connection.generation != generation) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: persistent collaborator runtime is unavailable");
  }
  if (kind == "close") {
    const std::string request = session::RandomToken(16);
    json command = {{"v", session::kProtocol},  {"session_id", HashHex(path)},
                    {"generation", generation}, {"kind", kind},
                    {"request_id", request},    {"client_request_id", request}};
    return session::WriteFrame(connection.socket.Get(), command)
               ? ToolSuccess("close accepted for " + id)
               : ToolFailure(ToolErrorCode::kUnavailable,
                             "error: collaborator connection closed");
  }
  json result = Command(path, generation, connection, kind, text,
                        Clock::now() + std::chrono::seconds(2), false);
  if (result.contains("command_error")) {
    return ToolFailure(
        JsonValue(result, "uncertain", false) ? ToolErrorCode::kProcessFailed
                                              : ToolErrorCode::kUnavailable,
        "error: " + JsonValue(result, "command_error", "command failed"));
  }
  return ToolSuccess(kind == "guide" ? "guidance accepted by " + id
                                     : kind + " accepted for " + id);
}

ToolResult CollaboratorRuntime::Message(const std::string& id,
                                        const std::string& /*text*/) {
  // Mail-first delivery: the payload is already on disk via
  // WriteCollaboratorMail, so this is only a wake-up ping. Empty text keeps
  // the child's SessionWorker from queueing a duplicate alongside the drain.
  if (!Active(id)) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: collaborator is idle");
  }
  return Control(id, "guide", "");
}

ToolResult CollaboratorRuntime::Interrupt(const std::string& id) {
  return Control(id, "interrupt");
}

ToolResult CollaboratorRuntime::Stop(const std::string& id) {
  State& state = *state_;
  std::string path, identity;
  pid_t pid = -1;
  {
    std::lock_guard lock(state.mutex);
    auto it = state.slots.find(id);
    if (it == state.slots.end()) {
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: persistent collaborator is not live");
    }
    path = it->second.path;
    pid = it->second.pid;
    identity = it->second.identity;
  }
  ToolResult result = Control(id, "close");
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (!path.empty() && Clock::now() < deadline &&
         session::Connect(path).socket) {
    poll(nullptr, 0, 20);
  }
  const auto owned = [&] {
    const std::string current = ProcessIdentity(pid);
    return pid > 0 && !identity.empty() &&
           (current.empty() || current == identity) && ProcessGroupAlive(pid);
  };
  if (owned()) {
    SignalProcessGroup(pid, SIGTERM);
    const auto grace = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < grace && owned()) poll(nullptr, 0, 20);
    if (owned()) SignalProcessGroup(pid, SIGKILL);
  }
  bool removed = false;
  {
    std::lock_guard lock(state.mutex);
    removed = state.slots.erase(id) > 0;
  }
  if (removed) PublishCollaborator({{"id", id}}, true);
  return removed ? ToolSuccess("stopped persistent collaborator " + id)
                 : result;
}

json CollaboratorRuntime::Snapshot(const std::string& id) const {
  const State& state = *state_;
  std::lock_guard lock(state.mutex);
  const State::Slot* slot = nullptr;
  if (!id.empty()) {
    auto it = state.slots.find(id);
    if (it == state.slots.end()) return json::object();
    slot = &it->second;
  } else {
    if (state.slots.empty()) return json::object();
    slot = &state.slots.begin()->second;
  }
  json result = {{"id", id.empty() ? state.slots.begin()->first : id},
                 {"label", slot->label},
                 {"model", slot->model},
                 {"persistent", true},
                 {"status", slot->active ? "running" : "idle"},
                 {"runtime_generation", slot->generation},
                 {"handoff_id", slot->handoff},
                 {"handoff_generation", slot->handoff_generation},
                 {"pid", slot->pid},
                 {"route", slot->route}};
  const std::string prompt = JsonValue(slot->latest, "system_prompt", "");
  if (!prompt.empty()) result["system_prompt"] = prompt;
  return result;
}

json CollaboratorRuntime::LiveView(const std::string& id) const {
  std::string path, generation;
  {
    const State& state = *state_;
    std::lock_guard lock(state.mutex);
    auto it = state.slots.find(id);
    if (it == state.slots.end() || !it->second.active) return json::object();
    path = it->second.path;
    generation = it->second.generation;
  }
  auto connection = session::Connect(path);
  if (!connection.socket || connection.generation != generation) {
    return json::object();
  }
  json state = Command(path, generation, connection, "refresh", "",
                       Clock::now() + std::chrono::seconds(2), false, nullptr,
                       json::object(), true);
  return state.contains("command_error")
             ? json::object()
             : JsonValue(state, "view", json::object());
}

bool CollaboratorRuntime::Active(const std::string& id) const {
  const State& state = *state_;
  std::lock_guard lock(state.mutex);
  auto it = state.slots.find(id);
  return it != state.slots.end() && it->second.active;
}

size_t CollaboratorRuntime::Count() const {
  const State& state = *state_;
  std::lock_guard lock(state.mutex);
  return state.slots.size();
}

void CollaboratorRuntime::Shutdown() {
  State& state = *state_;
  std::vector<std::string> ids;
  {
    std::lock_guard lock(state.mutex);
    if (state.shutdown) return;
    state.shutdown = true;
    ids.reserve(state.slots.size());
    for (const auto& [key, _] : state.slots) ids.push_back(key);
  }
  for (const std::string& id : ids) Stop(id);
}

}  // namespace uagent
