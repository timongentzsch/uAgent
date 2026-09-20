// Copyright 2026 Timon Gentzsch

#include "include/tools/tool.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/time.h"

namespace uagent {

const char* ToolErrorCodeName(ToolErrorCode code) {
  switch (code) {
    case ToolErrorCode::kNone:
      return "none";
    case ToolErrorCode::kInvalidArguments:
      return "invalid_arguments";
    case ToolErrorCode::kPermissionDenied:
      return "permission_denied";
    case ToolErrorCode::kNotFound:
      return "not_found";
    case ToolErrorCode::kLimitExceeded:
      return "limit_exceeded";
    case ToolErrorCode::kUnavailable:
      return "unavailable";
    case ToolErrorCode::kProcessFailed:
      return "process_failed";
    case ToolErrorCode::kRemoteError:
      return "remote_error";
    case ToolErrorCode::kInternal:
      return "internal";
  }
  return "internal";
}

std::string ToolErrorText(std::string_view message) {
  std::string out(kToolErrorPrefix);
  out += message;
  return out;
}

ToolArgumentIssue ArgumentIssue(std::string code, std::string message,
                                std::string field) {
  return {std::move(code), std::move(message), std::move(field)};
}

std::string ArtifactHint(const ToolArtifact& artifact) {
  return "\n[captured log: " + artifact.path + " (" +
         std::to_string(artifact.bytes) +
         " bytes); query with jq/python via run or read selected ranges; do "
         "not read it whole]";
}

ToolResult ToolSuccess(std::string output, int64_t result_chars) {
  ToolResult result;
  result.output = std::move(output);
  result.result_chars = result_chars;
  return result;
}

ToolResult ToolFailure(ToolErrorCode error, std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kFailed;
  result.output = std::move(output);
  result.error = error;
  return result;
}

ToolResult ToolCancelled(std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kCancelled;
  result.output = std::move(output);
  return result;
}

ToolResult ToolTimedOut(std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kTimedOut;
  result.output = std::move(output);
  return result;
}

bool ToolContext::Expired() const {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

ToolContext ToolContext::WithTimeout(int64_t seconds) const {
  ToolContext out = *this;
  out.timeout_s = std::max(int64_t{0}, seconds);
  if (seconds > 0) {
    out.deadline = std::min(out.deadline, DeadlineAfter(seconds));
  }
  return out;
}

int64_t ToolContext::RemainingSeconds(int64_t configured) const {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return configured;
  }
  int64_t remaining =
      static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count());
  remaining = std::max(int64_t{1}, remaining);
  return configured > 0 ? std::min(configured, remaining) : remaining;
}

Tool MakeTool(std::string name, std::string description, json parameters,
              Tool::Run run) {
  Tool tool;
  tool.name = std::move(name);
  tool.description = std::move(description);
  tool.parameters = std::move(parameters);
  tool.run = std::move(run);
  return tool;
}

Tool& AddTool(std::vector<Tool>& tools, Tool tool) {
  tools.push_back(std::move(tool));
  return tools.back();
}

bool ToolMutates(const Tool& tool, const json& arguments) {
  return tool.mutating || (tool.mutates && tool.mutates(arguments));
}

// Repeat-guard exemption for Agent::ToolCallsWithinLimits: a call that
// deliberately blocks — activity wait_ms or run yield_ms — is waiting for
// something to finish, not stuck in a tight identical-call loop, so it
// resets the counter instead of tripping it.
bool ToolCallBlocks(const Tool& tool, const json& arguments) {
  if (tool.blocking_wait_default_ms >= 0 &&
      JsonValue(arguments, "wait_ms", tool.blocking_wait_default_ms) > 0) {
    return true;
  }
  return JsonValue(arguments, "yield_ms", int64_t{0}) > 0;
}

// Contract-defined for native operations; arbitrary execution may declare its
// purpose. Neither this label nor a successful exit proves absence of effects.
std::string ToolActivityCategory(const Tool& tool, const json& args) {
  if (tool.declared_intent) {
    std::string intent = JsonValue(args, "intent", "execute");
    return intent == "explore" || intent == "change" ? intent : "execute";
  }
  if (tool.capabilities & (Capability(ToolCapability::kExecute) |
                           Capability(ToolCapability::kDelegate))) {
    return "execute";
  }
  return ToolMutates(tool, args) ? "change" : "explore";
}

// The authority a call needs. A tool may escalate specific arguments; nothing
// can de-escalate below what the tool itself declares.
ApprovalClass RequiredApproval(const Tool& tool, const json& arguments) {
  if (tool.approval_class) {
    ApprovalClass escalated = tool.approval_class(arguments);
    if (escalated == ApprovalClass::kMandatoryHuman) return escalated;
  }
  bool required = ToolMutates(tool, arguments) ||
                  (tool.needs_approval && tool.needs_approval(arguments));
  return required ? ApprovalClass::kYoloEligibleMutation : ApprovalClass::kNone;
}

void KeepLeanTools(std::vector<Tool>& tools) {
  std::erase_if(tools,
                [](const Tool& tool) { return !tool.available_in_lean; });
}

std::string ToolDescription(const Tool& tool) {
  std::string s = tool.description;
  // Mark tools that actually overlap, so the base prompt's batching rule is
  // actionable.
  if (tool.parallel_safe) s += " Batchable with independent calls.";
  if (tool.max_calls_per_turn >= 0) {
    s += " Limit: " + std::to_string(tool.max_calls_per_turn) + "/turn.";
  }
  return s;
}

json ToolParameters(const Tool& tool) {
  if (tool.provider.starts_with("mcp:")) return tool.parameters;
  json parameters = tool.parameters;
  if (!parameters.is_object()) parameters = json::object();
  if (!parameters.contains("type")) parameters["type"] = "object";
  if (!parameters.contains("properties") ||
      !parameters["properties"].is_object()) {
    parameters["properties"] = json::object();
  }
  if (!parameters.contains("additionalProperties")) {
    parameters["additionalProperties"] = false;
  }
  return parameters;
}

// One-line display for a call: the tool's own formatter, else `path` (the
// common case), else the raw args. Shared by the approval prompt and the
// call trace so both name the same action the same way.
std::string ToolSummary(const Tool& t, const json& args) {
  if (t.summary) return t.summary(args);
  if (args.contains("path") && args["path"].is_string()) {
    return args["path"].get<std::string>();
  }
  return JsonDump(args);
}

const Tool* FindTool(const std::vector<Tool>& tools, const std::string& name) {
  for (auto& t : tools) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

std::string InvalidToolArgument(const Tool& tool, const json& args) {
  auto issue = FindToolArgumentIssue(tool, args);
  return issue ? issue->message : std::string();
}

json ToolSchema(const Tool& tool) {
  return {{"type", "function"},
          {"function",
           {{"name", tool.name},
            {"description", ToolDescription(tool)},
            {"parameters", ToolParameters(tool)}}}};
}

// registry -> the `tools` array for a chat request
json ToolSchemas(const std::vector<Tool>& tools) {
  json out = json::array();
  for (const Tool& tool : tools) {
    out.push_back(ToolSchema(tool));
  }
  return out;
}

}  // namespace uagent
