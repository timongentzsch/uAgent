// Copyright 2026 Timon Gentzsch

#include "include/tools/collaborator_runtime.h"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

#include "include/agent/conversation.h"
#include "include/agent/session_store.h"
#include "include/app/session.h"
#include "include/core/events.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/jobs.h"

namespace uagent {
namespace {

using Clock = std::chrono::steady_clock;

Usage Difference(const Usage& current, const Usage& prior) {
  Usage result;
  result.input = std::max(int64_t{0}, current.input - prior.input);
  result.output = std::max(int64_t{0}, current.output - prior.output);
  result.cache_read =
      std::max(int64_t{0}, current.cache_read - prior.cache_read);
  result.cache_write =
      std::max(int64_t{0}, current.cache_write - prior.cache_write);
  result.reasoning = std::max(int64_t{0}, current.reasoning - prior.reasoning);
  result.web_searches =
      std::max(int64_t{0}, current.web_searches - prior.web_searches);
  result.cost = std::max(0.0, current.cost - prior.cost);
  result.cost_reported = current.cost_reported;
  return result;
}

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
             bool* submitted = nullptr, const json& budget = json::object()) {
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
  const bool wait_idle = JsonValue(command, "kind", "") == "refresh";
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
  explicit State(UsageAccumulator& target) : usage(target) {}

  void Account(const json& snapshot) {
    const Usage current = UsageFromJson(JsonValue(snapshot, "usage", json{}));
    const RouteUsage current_routes =
        RouteUsageFromJson(JsonValue(snapshot, "route_usage", json{}));
    if (current_routes.empty()) {
      usage.Add(Difference(current, accounted));
    } else {
      for (const auto& [route_name, value] : current_routes) {
        auto prior = accounted_routes.find(route_name);
        usage.Add(route_name, Difference(value, prior == accounted_routes.end()
                                                    ? Usage{}
                                                    : prior->second));
      }
    }
    accounted = current;
    accounted_routes = current_routes;
  }

  UsageAccumulator& usage;
  mutable std::mutex mutex;
  std::string id, path, generation, label, model, route, handoff, identity;
  Fd owner;
  pid_t pid = -1;
  uint64_t handoff_generation = 0;
  bool active = false;
  bool shutdown = false;
  Usage accounted;
  RouteUsage accounted_routes;
  json latest = json::object();
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
    if (!state.id.empty() && state.id != launch.id) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: persistent collaborator " + state.id +
                             " already owns this conversation's runtime");
    }
    if (state.active) {
      return ToolFailure(ToolErrorCode::kUnavailable,
                         "error: collaborator " + launch.id +
                             " already has an active handoff");
    }
    connection = session::Connect(launch.path);
    runtime_lost =
        !state.id.empty() &&
        (!connection.socket || connection.generation != state.generation);
    id = state.id;
    path = state.path;
    if (runtime_lost) state.active = false;
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
      if (state.id.empty()) {
        if (auto loaded = SessionStore::Inspect(launch.path); loaded.record) {
          state.accounted = loaded.record->state.usage;
          state.accounted_routes = loaded.record->state.route_usage;
        }
      }
      state.id = std::move(launch.id);
      state.path = std::move(launch.path);
      state.generation = connection.generation;
      if (connection.owner) state.owner = std::move(connection.owner);
      state.pid = connection.pid;
      state.identity = ProcessIdentity(connection.pid);
      state.label = std::move(launch.title);
      state.model = std::move(launch.model);
      state.route = std::move(launch.route);
      state.active = true;
      request_generation = session::RandomToken(16);
      state.handoff = request_generation;
      ++state.handoff_generation;
      id = state.id;
      path = state.path;
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
      state.Account(idle);
    }
    if (!retained) Stop(id);
    if (retained) {
      std::lock_guard lock(state.mutex);
      if (state.handoff == request_generation) state.active = false;
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
    if (state.handoff != request_generation) {
      return ToolCancelled("error: collaborator handoff was superseded");
    }
    state.active = false;
    state.latest = result;
    state.Account(result);
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
    if (state.id != id || state.path.empty()) {
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: persistent collaborator is not live");
    }
    path = state.path;
    generation = state.generation;
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
                                        const std::string& text) {
  if (!Active(id)) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: collaborator is idle");
  }
  return Control(id, "guide", "[parent guidance]\n" + text);
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
    if (state.id != id) {
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: persistent collaborator is not live");
    }
    path = state.path;
    pid = state.pid;
    identity = state.identity;
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
    if (state.id == id) {
      state.id.clear();
      state.path.clear();
      state.generation.clear();
      state.label.clear();
      state.model.clear();
      state.route.clear();
      state.handoff.clear();
      state.owner.Reset();
      state.pid = -1;
      state.identity.clear();
      state.active = false;
      state.accounted = {};
      state.accounted_routes.clear();
      state.latest = json::object();
      removed = true;
    }
  }
  if (removed) PublishCollaborator({{"id", id}}, true);
  return removed ? ToolSuccess("stopped persistent collaborator " + id)
                 : result;
}

json CollaboratorRuntime::Snapshot(const std::string& id) const {
  const State& state = *state_;
  std::lock_guard lock(state.mutex);
  if (state.id.empty() || (!id.empty() && id != state.id)) {
    return json::object();
  }
  json result = {{"id", state.id},
                 {"label", state.label},
                 {"model", state.model},
                 {"persistent", true},
                 {"status", state.active ? "running" : "idle"},
                 {"runtime_generation", state.generation},
                 {"handoff_id", state.handoff},
                 {"handoff_generation", state.handoff_generation},
                 {"pid", state.pid},
                 {"route", state.route}};
  const std::string prompt = JsonValue(state.latest, "system_prompt", "");
  if (!prompt.empty()) result["system_prompt"] = prompt;
  return result;
}

bool CollaboratorRuntime::Active(const std::string& id) const {
  const State& state = *state_;
  std::lock_guard lock(state.mutex);
  return state.id == id && state.active;
}

void CollaboratorRuntime::Shutdown() {
  State& state = *state_;
  std::string id;
  {
    std::lock_guard lock(state.mutex);
    if (state.shutdown) return;
    state.shutdown = true;
    id = state.id;
  }
  if (!id.empty()) Stop(id);
}

}  // namespace uagent
