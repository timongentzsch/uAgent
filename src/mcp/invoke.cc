// Copyright 2026 Timon Gentzsch

#include "include/mcp/invoke.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>

#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/mcp/elicit.h"
#include "include/mcp/result.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

namespace {

constexpr int64_t kTaskPollDefaultMs = 1000;
constexpr int64_t kTaskPollFloorMs = 250;
constexpr int64_t kTaskPollCeilingMs = 60000;

bool TaskEnded(const json& task) {
  const std::string status = JsonValue(task, "status", "");
  return status == "completed" || status == "failed" || status == "cancelled";
}

// Waits out a poll interval, still taking the server's notifications; false
// when the call was stopped or the server exited.
bool AwaitPoll(McpServer& server, std::chrono::steady_clock::time_point until) {
  std::string line;
  while (McpReadLine(server, line, until, true)) {
    const json message = json::parse(line, nullptr, false);
    if (!message.is_discarded()) McpHandleMessage(server, message);
  }
  return server.alive && !AbortRequested();
}

// Polls a task until it ends; the task at its end, or an error reply.
json FollowTask(McpServer& server, json task, int64_t timeout,
                const ToolContext& context, std::string& note) {
  const std::string id = JsonValue(task, "taskId", "");
  std::set<std::string> answered;
  std::string shown;
  auto request = [&](const char* method, const json& params) {
    const auto deadline = std::min(context.deadline, DeadlineAfter(timeout));
    return McpRpc(server, method, params, SecondsUntil(deadline), true,
                  deadline);
  };
  while (!TaskEnded(task)) {
    const std::string status = JsonValue(task, "status", "");
    const std::string message = JsonValue(task, "statusMessage", "");
    if (context.progress && !message.empty() && message != shown) {
      shown = message;
      context.progress(server.name + ": " + message);
    }
    if (status == "input_required") {
      // Keys stay unique for a task, and a poll may repeat one just answered.
      const json requested = JsonValue(task, "inputRequests", json());
      json pending = json::object();
      for (const auto& [key, input] : requested.items()) {
        if (!answered.contains(key)) pending[key] = input;
      }
      json responses = json::object();
      bool wait = false;
      if (!requested.is_object() ||
          !McpInputResponses(server, pending, responses, wait, note)) {
        return McpErrorReply("task requires unsupported input");
      }
      if (AbortRequested()) return McpErrorReply("cancelled");
      if (!pending.empty()) {
        for (const auto& [key, _] : pending.items()) answered.insert(key);
        json ack = request("tasks/update",
                           {{"taskId", id}, {"inputResponses", responses}});
        if (!ack.contains("result")) return ack;
      }
    } else if (status != "working") {
      return McpErrorReply("task has unknown status `" + status + "`");
    }
    const auto interval =
        std::clamp(JsonValue(task, "pollIntervalMs", kTaskPollDefaultMs),
                   kTaskPollFloorMs, kTaskPollCeilingMs);
    if (!AwaitPoll(server, std::min(context.deadline,
                                    std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(interval)))) {
      return McpErrorReply(server.alive
                               ? "cancelled"
                               : "server exited" + McpStderrHint(server.name));
    }
    if (SecondsUntil(context.deadline) <= 0) {
      return McpErrorReply("task did not end before the call's deadline");
    }
    json reply = request("tasks/get", {{"taskId", id}});
    const json* got = JsonObject(reply, "result");
    if (!got) return reply;
    if (JsonValue(*got, "resultType", "") != "complete" ||
        JsonValue(*got, "taskId", "") != id) {
      return McpErrorReply("tasks/get returned another shape");
    }
    task = *got;
  }
  return task;
}

// Cancellation is cooperative and needs no acknowledgement, so this does not
// wait for one; it is sent even while a stop is pending.
void CancelTask(McpServer& server, const std::string& id) {
  const bool stopping = AbortRequested();
  if (stopping) ClearAbort();
  McpSend(server, server.next_id++, "tasks/cancel", {{"taskId", id}},
          std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
  if (stopping) RequestAbort();
}

}  // namespace

ToolResult McpInvokeRemote(McpServer& server, const std::string& remote_name,
                           const json& arguments, int64_t timeout,
                           const ToolContext& context) {
  if (!server.alive) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "mcp server " + server.name + " has exited" +
                           McpStderrHint(server.name));
  }
  json response;
  std::string note;
  if (RunCancellable([&] {
        json params = {{"name", remote_name}, {"arguments", arguments}};
        bool wait = false;
        for (int round = 0; round < 8; ++round) {
          // Each request has its own budget, so a person's time answering is
          // never the server's; a retry after an accepted link has none.
          const auto deadline =
              wait ? context.deadline
                   : std::min(context.deadline, DeadlineAfter(timeout));
          response = McpRpc(server, "tools/call", params,
                            SecondsUntil(deadline), true, deadline);
          const json* result = JsonObject(response, "result");
          if (!result ||
              JsonValue(*result, "resultType", "") != "input_required" ||
              AbortRequested()) {
            return;
          }
          if (!result->contains("requestState") &&
              !result->contains("inputRequests")) {
            return;
          }
          params.erase("requestState");
          params["inputResponses"] = json::object();
          if (result->contains("requestState")) {
            if (!(*result)["requestState"].is_string()) return;
            params["requestState"] = (*result)["requestState"];
          }
          wait = false;
          if (const json* inputs = JsonObject(*result, "inputRequests")) {
            if (!McpInputResponses(server, *inputs, params["inputResponses"],
                                   wait, note)) {
              return;
            }
          } else if (result->contains("inputRequests")) {
            return;
          }
          if (AbortRequested() || SecondsUntil(context.deadline) <= 0) return;
        }
      })) {
    return ToolCancelled("error: call cancelled by user");
  }
  const json* created = JsonObject(response, "result");
  if (created && JsonValue(*created, "resultType", "") == "task") {
    const std::string id = JsonValue(*created, "taskId", "");
    if (id.empty()) {
      return ToolFailure(ToolErrorCode::kRemoteError,
                         "mcp(" + server.name + "): task without a taskId");
    }
    // A created task carries no result or input requests; tasks/get does.
    json task = *created;
    task["status"] = "working";
    const bool stopped = RunCancellable(
        [&] { task = FollowTask(server, task, timeout, context, note); });
    if (!TaskEnded(task)) CancelTask(server, id);
    if (stopped) return ToolCancelled("error: call cancelled by user");
    const std::string status = JsonValue(task, "status", "");
    if (status == "cancelled") {
      const std::string why = JsonValue(task, "statusMessage", "");
      return ToolCancelled("error: mcp(" + server.name +
                           ") cancelled the task" +
                           (why.empty() ? "" : ": " + why));
    }
    response = status == "completed" ? json{{"result", task["result"]}}
               : status == "failed"
                   ? json{{"error", JsonValue(task, "error", json::object())}}
                   : task;
  }
  ToolResult result = McpResultText(server, response, context.call_id);
  AddHint(result, Trim(note));
  return result;
}

}  // namespace uagent
