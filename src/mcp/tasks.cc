// Copyright 2026 Timon Gentzsch

#include "include/mcp/tasks.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "include/agent/activity.h"
#include "include/agent/activity_supervisor.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/workspace.h"
#include "include/mcp/elicit.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kTaskPollDefaultMs = 1000;
constexpr int64_t kTaskPollFloorMs = 250;
constexpr int64_t kTaskPollCeilingMs = 60000;
// A background poll runs on the session's own thread: a slow server costs a
// turn at most this, and is asked again at its next poll.
constexpr int64_t kBackgroundGetSeconds = 3;
// How often a waiting call looks for Ctrl+B or guidance.
constexpr auto kHandoverCheck = std::chrono::milliseconds(250);

bool TaskEnded(const json& task) {
  const std::string status = JsonValue(task, "status", "");
  return status == "completed" || status == "failed" || status == "cancelled";
}

std::chrono::milliseconds Interval(const json& task) {
  return std::chrono::milliseconds(
      std::clamp(JsonValue(task, "pollIntervalMs", kTaskPollDefaultMs),
                 kTaskPollFloorMs, kTaskPollCeilingMs));
}

Clock::time_point NextPoll(const json& task) {
  return Clock::now() + Interval(task);
}

// A background request is not the call's to stop: a person's Stop is aimed
// at the turn, so it leaves the task for its next poll.
json Request(McpServer& server, const char* method, const json& params,
             Clock::time_point deadline, bool cancellable) {
  return McpRpc(server, method, params, SecondsUntil(deadline), cancellable,
                deadline);
}

// Waits, still taking the server's notifications; false when the call was
// stopped or the server exited.
bool AwaitPoll(McpServer& server, Clock::time_point until) {
  std::string line;
  while (McpReadLine(server, line, until, true)) {
    const json message = json::parse(line, nullptr, false);
    if (!message.is_discarded()) McpHandleMessage(server, message);
  }
  return server.alive && !AbortRequested();
}

// The task's status message when it changed, as "server: message".
std::optional<std::string> NewStatus(const McpServer& server, McpTask& t) {
  const std::string message = JsonValue(t.task, "statusMessage", "");
  if (message.empty() || message == t.shown) return std::nullopt;
  t.shown = message;
  return server.name + ": " + message;
}

// The activity row's progress line.
void ShowOnRow(McpTask& t, const std::string& status,
               ActivitySupervisor& processes) {
  {
    std::lock_guard lock(t.session->mutex);
    t.session->transcript.Push(OneLine(status) + "\n");
  }
  processes.Wake();
}

// Answers the task's new input requests through tasks/update; an error reply
// when it cannot go on.
std::optional<json> Answer(McpServer& server, McpTask& t,
                           Clock::time_point deadline, bool background,
                           std::string& note) {
  if (JsonValue(t.task, "status", "") != "input_required") return std::nullopt;
  // Keys stay unique for a task, and a poll may repeat one just answered.
  const json requested = JsonValue(t.task, "inputRequests", json());
  json pending = json::object();
  for (const auto& [key, input] : requested.items()) {
    if (!t.answered.contains(key)) pending[key] = input;
  }
  json responses = json::object();
  bool wait = false;
  if (!requested.is_object() ||
      !McpInputResponses(server, pending, responses, wait, note)) {
    return McpErrorReply("task requires unsupported input");
  }
  // A stop closes the card: in the background it is asked again next poll.
  if (AbortRequested()) {
    return background ? std::nullopt
                      : std::optional(McpErrorReply("cancelled"));
  }
  if (pending.empty()) return std::nullopt;
  for (const auto& [key, _] : pending.items()) t.answered.insert(key);
  json ack = Request(server, "tasks/update",
                     {{"taskId", t.id}, {"inputResponses", responses}},
                     deadline, !background);
  return ack.contains("result") ? std::nullopt : std::optional(ack);
}

// tasks/get into `t.task`; an error reply when it fails.
std::optional<json> Refresh(McpServer& server, McpTask& t,
                            Clock::time_point deadline, bool cancellable) {
  json reply =
      Request(server, "tasks/get", {{"taskId", t.id}}, deadline, cancellable);
  const json* got = JsonObject(reply, "result");
  if (!got) return reply;
  if (JsonValue(*got, "resultType", "") != "complete" ||
      JsonValue(*got, "taskId", "") != t.id) {
    return McpErrorReply("tasks/get returned another shape");
  }
  const std::string status = JsonValue(*got, "status", "");
  if (!TaskEnded(*got) && status != "working" && status != "input_required") {
    return McpErrorReply("task has unknown status `" + status + "`");
  }
  t.task = *got;
  return std::nullopt;
}

// Cancellation is cooperative and needs no acknowledgement, so this does not
// wait for one; it is sent even while a stop is pending.
void CancelTask(McpServer& server, const std::string& id) {
  const bool stopping = AbortRequested();
  if (stopping) ClearAbort();
  McpSend(server, server.next_id++, "tasks/cancel", {{"taskId", id}},
          Clock::now() + std::chrono::milliseconds(50));
  if (stopping) RequestAbort();
}

// What a call reports for `ended`: a task at its end, or an error reply.
ToolResult EndedResult(const McpServer& server, const json& ended,
                       const std::string& call_id) {
  const std::string status = JsonValue(ended, "status", "");
  if (status == "cancelled") {
    const std::string why = JsonValue(ended, "statusMessage", "");
    return ToolCancelled("error: mcp(" + server.name + ") cancelled the task" +
                         (why.empty() ? "" : ": " + why));
  }
  return McpResultText(
      server,
      status == "completed"
          ? json{{"result", JsonValue(ended, "result", json())}}
      : status == "failed"
          ? json{{"error", JsonValue(ended, "error", json::object())}}
          : ended,
      call_id);
}

McpServer* LiveServer(McpRuntime& runtime, const std::string& name) {
  for (const auto& server : runtime.Servers()) {
    if (server->alive && server->name == name) return server.get();
  }
  return nullptr;
}

// Ends a background task's activity; its result reaches the conversation
// through the activity's completion.
void Finish(McpRuntime& runtime, McpTask& t, const ToolResult* result) {
  ActivitySupervisor& processes = *runtime.processes;
  if (!result) {
    {
      std::lock_guard lock(t.session->mutex);
      (void)TransitionActivityLocked(*t.session, ActivityState::kStopped);
    }
    if (std::optional<Activity> job = processes.Take(t.activity, true)) {
      processes.Stopped(
          {.activity_id = t.activity,
           .duration_ms = static_cast<double>(NowMillis() - job->started_ms),
           .stopped = true});
    }
    return;
  }
  {
    std::lock_guard lock(t.session->mutex);
    const std::string text = ModelText(*result);
    t.session->pending_output.Push(text);
    t.session->transcript.Push(text);
    t.session->wait_status = result->Ok() ? 0 : 1 << 8;  // exit status 1
    t.session->exited_at = Clock::now();
    (void)TransitionActivityLocked(*t.session, ActivityState::kExited);
    (void)TransitionActivityLocked(*t.session, ActivityState::kDrained);
  }
  processes.Wake();
}

}  // namespace

ToolResult McpFollowTask(McpServer& server, const json& created,
                         const std::string& label, int64_t timeout,
                         const ToolContext& context, std::string& note) {
  McpTask t{.server = server.name,
            .id = JsonValue(created, "taskId", ""),
            .call_id = context.call_id,
            .timeout = timeout,
            .task = created};
  if (t.id.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "mcp(" + server.name + "): task without a taskId");
  }
  // A created task carries no result or input requests; tasks/get does.
  t.task["status"] = "working";
  // Only a conversation returns to a task's result; a headless run waits.
  // A remote workspace's activity controls reach its host, never this task.
  ActivitySupervisor* processes =
      server.runtime ? server.runtime->processes : nullptr;
  if (processes && processes->WakesSession() && !RemoteWorkspace()) {
    if (auto reservation = processes->ReserveActivity(MaxActivities())) {
      t.session = std::make_shared<ActivitySession>();
      const std::optional<int64_t> id =
          reservation->Register({.cmd = label,
                                 .kind = ActivityKind::kMcpTask,
                                 .id = reservation->Id(),
                                 .session = t.session,
                                 .display_label = label});
      if (!id) t.session.reset();
      t.activity = id.value_or(0);
      // Starts the supervisor's loop, which publishes the activity's row.
      processes->WakeAt(Clock::now());
    }
  }
  const auto yield = Clock::now() + std::chrono::milliseconds(kDefaultYieldMs);
  bool handed_off = false;
  bool person_stopped = false;
  auto handover = [&] {
    if (!t.session) return false;
    {
      std::lock_guard lock(t.session->mutex);
      handed_off = t.session->background_requested;
      person_stopped = t.session->stop_requested;
    }
    handed_off = handed_off || SteeringYieldRequested();
    return person_stopped || handed_off || Clock::now() >= yield;
  };
  bool moved = false;
  json ended;
  const bool stopped = RunCancellable([&] {
    auto request_deadline = [&] {
      return std::min(context.deadline, DeadlineAfter(timeout));
    };
    while (!TaskEnded(t.task)) {
      if (auto status = NewStatus(server, t)) {
        if (context.progress) context.progress(*status);
        if (t.session) ShowOnRow(t, *status, *processes);
      }
      if (auto error = Answer(server, t, request_deadline(), false, note)) {
        ended = *error;
        return;
      }
      const auto due = std::min(context.deadline, NextPoll(t.task));
      while (Clock::now() < due) {
        if (handover()) {
          moved = !person_stopped;
          return;
        }
        if (!AwaitPoll(server, std::min(due, Clock::now() + kHandoverCheck))) {
          ended = McpErrorReply(server.alive ? "cancelled"
                                             : "server exited" +
                                                   McpStderrHint(server.name));
          return;
        }
      }
      if (SecondsUntil(context.deadline) <= 0) {
        ended = McpErrorReply("task did not end before the call's deadline");
        return;
      }
      if (auto error = Refresh(server, t, request_deadline(), true)) {
        ended = *error;
        return;
      }
    }
    ended = t.task;
  });
  // Only shutdown takes the foreground entry away; the task then ends.
  if (moved && !stopped && processes &&
      processes->MoveForegroundToBackground(t.activity)) {
    t.next = NextPoll(t.task);
    processes->WakeAt(t.next);
    const int64_t activity = t.activity;
    server.runtime->tasks.push_back(std::move(t));
    return RunningActivityResult(*processes, activity, handed_off);
  }
  if (t.session && processes) (void)processes->RemoveForeground(t.activity);
  if (!TaskEnded(ended)) CancelTask(server, t.id);
  if (stopped || person_stopped || moved) {
    return ToolCancelled("error: call cancelled by user");
  }
  return EndedResult(server, ended, context.call_id);
}

void McpPollTasks(McpRuntime& runtime, bool ask) {
  auto next = Clock::time_point::max();
  for (auto it = runtime.tasks.begin(); it != runtime.tasks.end();) {
    McpTask& t = *it;
    // A pending stop is the turn's; this task is polled after it.
    if (AbortRequested()) {
      next = std::min(next, t.next);
      ++it;
      continue;
    }
    McpServer* server = LiveServer(runtime, t.server);
    bool stop = false;
    {
      std::lock_guard lock(t.session->mutex);
      stop = t.session->stop_requested;
    }
    std::optional<json> ended;
    if (!server) {
      ended = json::object();
    } else if (stop) {
      CancelTask(*server, t.id);
      Finish(runtime, t, nullptr);
      it = runtime.tasks.erase(it);
      continue;
    } else if (Clock::now() >= t.next) {
      const auto deadline = DeadlineAfter(kBackgroundGetSeconds);
      std::optional<json> error = Refresh(*server, t, deadline, false);
      if (error && server->alive &&
          (Clock::now() >= deadline || AbortRequested())) {
        // Not this time: a late reply is skipped when it comes, and a server
        // that keeps not answering is asked less and less often.
        t.backoff = std::min(std::chrono::milliseconds(kTaskPollCeilingMs),
                             2 * std::max(t.backoff, Interval(t.task)));
        t.next = Clock::now() + t.backoff;
      } else {
        t.backoff = {};
        t.next = NextPoll(t.task);
        if (error) {
          ended = *error;
        } else if (TaskEnded(t.task)) {
          ended = t.task;
        } else if (auto status = NewStatus(*server, t)) {
          ShowOnRow(t, *status, *runtime.processes);
        }
      }
    }
    // Input is asked only where a person can be waited for: the runtime's
    // own step and idle paths, never a tool's wait.
    const bool asks =
        !ended && server && JsonValue(t.task, "status", "") == "input_required";
    if (asks && ask) {
      ended = Answer(*server, t, DeadlineAfter(t.timeout), true, t.note);
    }
    {
      std::lock_guard lock(t.session->mutex);
      t.session->waiting_for_person = asks && !ask;
    }
    if (ended) {
      if (server && !TaskEnded(*ended)) CancelTask(*server, t.id);
      ToolResult result =
          server ? EndedResult(*server, *ended, t.call_id)
                 : ToolFailure(ToolErrorCode::kUnavailable,
                               "mcp server " + t.server + " has exited" +
                                   McpStderrHint(t.server));
      AddHint(result, Trim(t.note));
      Finish(runtime, t, &result);
      it = runtime.tasks.erase(it);
      continue;
    }
    next = std::min(next, t.next);
    ++it;
  }
  if (next != Clock::time_point::max()) runtime.processes->WakeAt(next);
}

void McpCancelTasks(McpRuntime& runtime) {
  for (const McpTask& t : runtime.tasks) {
    if (McpServer* server = LiveServer(runtime, t.server)) {
      CancelTask(*server, t.id);
    }
  }
}

}  // namespace uagent
