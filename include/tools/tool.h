// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_TOOL_H_
#define UAGENT_INCLUDE_TOOLS_TOOL_H_
// The Tool type and registry machinery. Each Tool bundles a canonical function
// schema with its handler; wire adapters translate it at the API boundary.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/outcome.h"
#include "include/core/time.h"

namespace uagent {

enum class ToolErrorCode {
  kNone,
  kInvalidArguments,
  kPermissionDenied,
  kNotFound,
  kLimitExceeded,
  kUnavailable,
  kProcessFailed,
  kRemoteError,
  kInternal,
};

inline const char* ToolErrorCodeName(ToolErrorCode code) {
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

struct ToolArgumentIssue {
  std::string code;
  std::string message;
  std::string field;
};

inline ToolArgumentIssue ArgumentIssue(std::string code, std::string message,
                                       std::string field = {}) {
  return {std::move(code), std::move(message), std::move(field)};
}

struct ToolArtifact {
  std::string path;
  uint64_t bytes = 0;
};

inline std::string ArtifactHint(const ToolArtifact& artifact) {
  return "\n[captured log: " + artifact.path + " (" +
         std::to_string(artifact.bytes) +
         " bytes); query with jq/python via run or read selected ranges; do "
         "not read it whole]";
}

struct ReadRange {
  std::string path;
  int64_t first = 0, last = 0;
};

struct ToolResult {
  CompletionStatus status = CompletionStatus::kSuccess;
  std::string output;
  ToolErrorCode error = ToolErrorCode::kNone;
  std::optional<ToolArtifact> artifact;
  std::optional<ReadRange> read_range;
  // Optional model-facing override for this call. Most tools inherit their
  // registry cap; a bounded richer result can raise it.
  int64_t result_chars = -1;
  std::string display;     // optional terminal-only receipt
  bool no_change = false;  // activity poll found nothing new
  bool activity_terminal = false;

  bool Ok() const { return status == CompletionStatus::kSuccess; }
};

inline ToolResult ToolSuccess(std::string output, int64_t result_chars = -1) {
  ToolResult result;
  result.output = std::move(output);
  result.result_chars = result_chars;
  return result;
}

inline ToolResult ToolFailure(ToolErrorCode error, std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kFailed;
  result.output = std::move(output);
  result.error = error;
  return result;
}

inline ToolResult ToolCancelled(std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kCancelled;
  result.output = std::move(output);
  return result;
}

inline ToolResult ToolTimedOut(std::string output) {
  ToolResult result;
  result.status = CompletionStatus::kTimedOut;
  result.output = std::move(output);
  return result;
}

struct ToolContext {
  ToolContext() = default;
  explicit ToolContext(std::chrono::steady_clock::time_point deadline_value)
      : deadline(deadline_value) {}

  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  bool image_input_available = true;
  bool image_fallback_available = false;
  int64_t timeout_s = 0;
  std::string call_id;

  bool Expired() const {
    return deadline != std::chrono::steady_clock::time_point::max() &&
           std::chrono::steady_clock::now() >= deadline;
  }

  ToolContext WithTimeout(int64_t seconds) const {
    ToolContext out = *this;
    out.timeout_s = std::max(int64_t{0}, seconds);
    if (seconds > 0) {
      out.deadline = std::min(out.deadline, DeadlineAfter(seconds));
    }
    return out;
  }

  int64_t RemainingSeconds(int64_t configured) const {
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
};

enum class ToolCapability : uint32_t {
  kInspect = 1U << 0,
  kExecute = 1U << 1,
  kMutate = 1U << 2,
  kDelegate = 1U << 3,
  kExternal = 1U << 4,
};

inline constexpr uint32_t Capability(ToolCapability capability) {
  return static_cast<uint32_t>(capability);
}

inline constexpr uint32_t kAllToolCapabilities =
    Capability(ToolCapability::kInspect) |
    Capability(ToolCapability::kExecute) | Capability(ToolCapability::kMutate) |
    Capability(ToolCapability::kDelegate) |
    Capability(ToolCapability::kExternal);

enum class ApprovalClass {
  kNone,
  kYoloEligibleMutation,
  kMandatoryHuman,
};

struct Tool {
  using Run = std::function<ToolResult(const json&, const ToolContext&)>;
  using Summary = std::function<std::string(const json&)>;
  using Approval = std::function<bool(const json&)>;
  using Validate = std::function<std::optional<ToolArgumentIssue>(const json&)>;
  using Canonicalize = std::function<void(json&)>;
  // How a call must be authorised. `kMandatoryHuman` exists because some
  // targets change µAgent's own authority or limits: yolo, UAGENT_APPROVAL and
  // /yolo do not apply to it, and a non-interactive session denies rather than
  // assuming consent.
  using Classify = std::function<ApprovalClass(const json&)>;
  // Full, possibly multi-line text shown only when asking a person to approve
  // this call. `summary` stays a one-liner for labels, traces and evidence.
  using Preview = std::function<std::string(const json&)>;

  std::string name;
  std::string description;
  json parameters;        // JSON-schema for the args
  bool mutating = false;  // gated behind user approval
  Approval mutates;       // argument-dependent mutation (e.g. memory save)
  Run run;
  Canonicalize canonicalize;  // materialized provider args -> operation args
  Validate validate;          // semantic issue before approval/execution
  Summary summary;            // args -> one-line display
  bool redact_invalid_arguments = false;  // hide raw rejected arguments
  bool parallel_safe = false;             // safe beside another tool call
  uint32_t capabilities = kAllToolCapabilities;  // required to expose
  Approval needs_approval;   // dynamic policy (e.g. external read)
  Classify approval_class;   // escalates specific arguments
  Preview approval_preview;  // long-form text for the approval prompt
  // Why this call needs a person, when the reason is not the default one of
  // reaching µAgent's own configuration. Shown in the approval headline.
  std::string mandatory_reason;
  std::string provider;             // owner for live registry refresh
  json output_schema;               // optional MCP output contract
  std::string stable_argument;      // value must stay fixed during one turn
  int64_t timeout_s = -1;           // -1 = global default; 0 = turn limit
  int64_t result_chars = -1;        // -1 = global result cap
  int64_t max_calls_per_turn = -1;  // -1 = global turn budget
  // Numeric pacing hints — a yield, a context radius, a page size — where a
  // value outside the schema's bounds means "as much as allowed", not a
  // mistake. Clamping them into range costs nothing; rejecting spends a whole
  // model round to say the same thing, and these are the arguments models
  // overshoot most often.
  std::vector<std::string> clamped_arguments;
  bool available_in_lean = true;  // omit implementation-only schemas in tasks
  bool retain_output = false;     // keep durable procedure/state in context
  bool dedupe_output = false;     // collapse verified recent duplicate output
  bool serial_media = false;      // suppress activity animation while rendering
  bool replay_image = false;      // replay a successful historical local image
  bool command_policy = false;    // receives the approved-command allowlist
  bool delegates = false;         // contributes delegation runtime context
  bool memory_store = false;      // retained only when memory is enabled
  int64_t blocking_wait_default_ms =
      -1;  // >=0: wait_ms is intentional blocking
  enum class Visibility {
    kAlways,
    kDetachedTerminal,
  };
  Visibility visibility = Visibility::kAlways;
};

struct ToolAvailability {
  bool detached_terminal = false;
};

struct ToolPolicy {
  uint32_t allowed = kAllToolCapabilities;
  std::vector<std::string> tool_allowlist;
  std::vector<std::string> run_allowlist;
  std::string error;
};

ToolPolicy ToolPolicyFromEnvironment();
void ApplyToolPolicy(std::vector<Tool>& tools, const ToolPolicy& policy);

inline Tool MakeTool(std::string name, std::string description, json parameters,
                     Tool::Run run) {
  Tool tool;
  tool.name = std::move(name);
  tool.description = std::move(description);
  tool.parameters = std::move(parameters);
  tool.run = std::move(run);
  return tool;
}

inline Tool& AddTool(std::vector<Tool>& tools, Tool tool) {
  tools.push_back(std::move(tool));
  return tools.back();
}

inline bool ToolMutates(const Tool& tool, const json& arguments) {
  return tool.mutating || (tool.mutates && tool.mutates(arguments));
}

// The authority a call needs. A tool may escalate specific arguments; nothing
// can de-escalate below what the tool itself declares.
inline ApprovalClass RequiredApproval(const Tool& tool, const json& arguments) {
  if (tool.approval_class) {
    ApprovalClass escalated = tool.approval_class(arguments);
    if (escalated == ApprovalClass::kMandatoryHuman) return escalated;
  }
  bool required = ToolMutates(tool, arguments) ||
                  (tool.needs_approval && tool.needs_approval(arguments));
  return required ? ApprovalClass::kYoloEligibleMutation : ApprovalClass::kNone;
}

inline void KeepLeanTools(std::vector<Tool>& tools) {
  std::erase_if(tools,
                [](const Tool& tool) { return !tool.available_in_lean; });
}

inline std::string ToolDescription(const Tool& tool) {
  std::string s = tool.description;
  // Mark tools that actually overlap, so the base prompt's batching rule is
  // actionable.
  if (tool.parallel_safe) s += " Batchable with independent calls.";
  if (tool.max_calls_per_turn >= 0) {
    s += " Limit: " + std::to_string(tool.max_calls_per_turn) + "/turn.";
  }
  return s;
}

inline json ToolParameters(const Tool& tool) {
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
inline std::string ToolSummary(const Tool& t, const json& args) {
  if (t.summary) return t.summary(args);
  if (args.contains("path") && args["path"].is_string()) {
    return args["path"].get<std::string>();
  }
  return JsonDump(args);
}

inline const Tool* FindTool(const std::vector<Tool>& tools,
                            const std::string& name) {
  for (auto& t : tools) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

// Structured argument validation against a tool's JSON schema.
std::optional<ToolArgumentIssue> FindToolArgumentIssue(const Tool& tool,
                                                       const json& args);

// Compatibility helper for callers that only need the human-readable error.
inline std::string InvalidToolArgument(const Tool& tool, const json& args) {
  auto issue = FindToolArgumentIssue(tool, args);
  return issue ? issue->message : std::string();
}

// Pull the tool's `clamped_arguments` back inside their schema bounds. Runs
// before validation, so an overshooting hint is honoured at the bound. Each
// reduction is appended to `clamped` when given, because a caller that asked
// for more than it got is owed both numbers rather than a quiet substitution.
void ClampToolArguments(const Tool& tool, json& args,
                        std::vector<std::string>* clamped = nullptr);

// Apply a tool's provider-materialization cleanup and numeric clamps to the
// separate execution copy of its arguments.
void CanonicalizeToolArguments(const Tool& tool, json& args,
                               std::vector<std::string>* clamped = nullptr);

// A tool's `stable_argument` must keep the same value for a whole turn.
// `values` carries that per-turn memory for the caller.
std::string StableArgumentError(
    const Tool& tool, const json& args,
    std::unordered_map<std::string, std::string>& values);

inline json ToolSchema(const Tool& tool) {
  return {{"type", "function"},
          {"function",
           {{"name", tool.name},
            {"description", ToolDescription(tool)},
            {"parameters", ToolParameters(tool)}}}};
}

// registry -> the `tools` array for a chat request
inline json ToolSchemas(const std::vector<Tool>& tools) {
  json out = json::array();
  for (const Tool& tool : tools) {
    out.push_back(ToolSchema(tool));
  }
  return out;
}

class ToolSchemaCache {
 public:
  void Reset() { valid_ = false; }

  const json& Get(const std::vector<Tool>& tools, const json& schemas,
                  const std::unordered_map<std::string, int64_t>& counts,
                  ToolAvailability availability = {}) {
    std::vector<size_t> selected;
    for (size_t i = 0; i < tools.size() && i < schemas.size(); ++i) {
      const Tool& tool = tools[i];
      if (tool.visibility == Tool::Visibility::kDetachedTerminal &&
          !availability.detached_terminal) {
        continue;
      }
      auto count = counts.find(tool.name);
      if (tool.max_calls_per_turn >= 0 && count != counts.end() &&
          count->second >= tool.max_calls_per_turn) {
        continue;
      }
      selected.push_back(i);
    }
    if (!valid_ || selected != selected_) {
      available_ = json::array();
      for (size_t i : selected) available_.push_back(schemas[i]);
      selected_ = std::move(selected);
      bytes_ = JsonEstimatedBytes(available_);
      valid_ = true;
    }
    return available_;
  }

  size_t Bytes() const { return bytes_; }
  const json& Schemas() const { return available_; }

 private:
  bool valid_ = false;
  size_t bytes_ = 0;
  std::vector<size_t> selected_;
  json available_ = json::array();
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_TOOL_H_
