// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_TOOL_H_
#define UAGENT_INCLUDE_TOOLS_TOOL_H_
// The Tool type and registry machinery. Each Tool bundles its OpenAI
// schema with its handler, so adding a capability means appending to the
// registry; nothing in the agent loop changes.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/outcome.h"
#include "include/core/time.h"

namespace uagent {

using nlohmann::json;

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

struct ToolResult {
  CompletionStatus status = CompletionStatus::kSuccess;
  std::string output;
  ToolErrorCode error = ToolErrorCode::kNone;
  std::optional<ToolArtifact> artifact;
  // Optional model-facing override for this call. Most tools inherit their
  // registry cap; a bounded richer result can raise it.
  int64_t result_chars = -1;
  std::string display;  // optional terminal-only receipt

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
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  int64_t timeout_s = 0;

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

struct Tool {
  using Run = std::function<ToolResult(const json&, const ToolContext&)>;
  using Summary = std::function<std::string(const json&)>;
  using Approval = std::function<bool(const json&)>;
  using Validate = std::function<std::string(const json&)>;

  std::string name;
  std::string description;
  json parameters;        // JSON-schema for the args
  bool mutating = false;  // gated behind user approval
  Approval mutates;       // argument-dependent mutation (e.g. memory save)
  Run run;
  Validate validate;           // args -> error before approval/execution
  Summary summary;             // args -> one-line display
  bool parallel_safe = false;  // safe beside another tool call
  uint32_t capabilities = kAllToolCapabilities;  // required to expose
  Approval needs_approval;          // dynamic policy (e.g. external read)
  std::string provider;             // owner for live registry refresh
  json output_schema;               // optional MCP output contract
  std::string stable_argument;      // value must stay fixed during one turn
  int64_t timeout_s = -1;           // -1 = global default; 0 = turn limit
  int64_t result_chars = -1;        // -1 = global result cap
  int64_t max_calls_per_turn = -1;  // -1 = global turn budget
  bool available_in_lean = true;    // omit implementation-only schemas in tasks
  bool retain_output = false;       // keep durable procedure/state in context
  bool dedupe_output = false;       // collapse verified recent duplicate output
  enum class Visibility {
    kAlways,
    kCheckpointHint,
    kDetachedTerminal,
  };
  Visibility visibility = Visibility::kAlways;
};

struct ToolAvailability {
  bool checkpoint_hint = false;
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

inline bool JsonSchemaTypeMatches(const json& value, std::string_view type) {
  return (type == "string" && value.is_string()) ||
         (type == "integer" && value.is_number_integer()) ||
         (type == "number" && value.is_number()) ||
         (type == "boolean" && value.is_boolean()) ||
         (type == "object" && value.is_object()) ||
         (type == "array" && value.is_array()) ||
         (type == "null" && value.is_null());
}

inline std::string JsonSchemaTypeLabel(const json& type) {
  if (type.is_string()) return type.get<std::string>();
  if (!type.is_array()) return "the expected type";
  std::string label;
  for (const json& item : type) {
    if (!item.is_string()) continue;
    if (!label.empty()) label += " or ";
    label += item.get_ref<const std::string&>();
  }
  return label.empty() ? "the expected type" : label;
}

inline std::optional<size_t> JsonSchemaSize(const json& schema,
                                            std::string_view name) {
  auto value = schema.find(name);
  if (value == schema.end()) return std::nullopt;
  if (value->is_number_unsigned()) {
    uint64_t size = value->get<uint64_t>();
    return size > std::numeric_limits<size_t>::max()
               ? std::nullopt
               : std::optional<size_t>(static_cast<size_t>(size));
  }
  if (!value->is_number_integer()) return std::nullopt;
  int64_t size = value->get<int64_t>();
  return size < 0 ? std::nullopt
                  : std::optional<size_t>(static_cast<size_t>(size));
}

inline std::string InvalidSchemaValue(const json& schema, const json& value,
                                      const std::string& path,
                                      bool root = false);

inline std::string InvalidArrayValue(const json& schema, const json& value,
                                     const std::string& path) {
  if (std::optional<size_t> minimum = JsonSchemaSize(schema, "minItems");
      minimum && value.size() < *minimum) {
    return "`" + path + "` has too few items";
  }
  if (std::optional<size_t> maximum = JsonSchemaSize(schema, "maxItems");
      maximum && value.size() > *maximum) {
    return "`" + path + "` has too many items";
  }
  auto items = schema.find("items");
  if (items == schema.end() || !items->is_object()) return "";
  for (size_t index = 0; index < value.size(); ++index) {
    std::string invalid = InvalidSchemaValue(
        *items, value[index], path + "[" + std::to_string(index) + "]");
    if (!invalid.empty()) return invalid;
  }
  return "";
}

inline std::string InvalidObjectValue(const json& schema, const json& value,
                                      const std::string& path, bool root) {
  auto required = schema.find("required");
  if (required != schema.end() && required->is_array()) {
    for (const json& item : *required) {
      if (!item.is_string()) continue;
      const std::string& name = item.get_ref<const std::string&>();
      if (value.contains(name)) continue;
      std::string child = root || path.empty() ? name : path + "." + name;
      return "`" + child + "` is required";
    }
  }

  auto found = schema.find("properties");
  const json properties =
      found != schema.end() && found->is_object() ? *found : json::object();
  if (!JsonValue(schema, "additionalProperties", true)) {
    for (const auto& [name, child] : value.items()) {
      (void)child;
      if (properties.contains(name)) continue;
      return "unknown argument `" + (root ? name : path + "." + name) + "`";
    }
  }
  for (const auto& [name, child_schema] : properties.items()) {
    if (!value.contains(name)) continue;
    std::string child = root || path.empty() ? name : path + "." + name;
    std::string invalid = InvalidSchemaValue(child_schema, value[name], child);
    if (!invalid.empty()) return invalid;
  }
  return "";
}

inline bool JsonSchemaTypesMatch(const json& value, const json& types) {
  if (types.is_string()) {
    return JsonSchemaTypeMatches(value, types.get_ref<const std::string&>());
  }
  if (!types.is_array()) return false;
  return std::any_of(types.begin(), types.end(), [&](const json& type) {
    return type.is_string() &&
           JsonSchemaTypeMatches(value, type.get_ref<const std::string&>());
  });
}

inline std::string InvalidSchemaValue(const json& schema, const json& value,
                                      const std::string& path, bool root) {
  if (!schema.is_object()) return "";
  auto types = schema.find("type");
  if (types != schema.end() && !JsonSchemaTypesMatch(value, *types)) {
    return "`" + path + "` must be " + JsonSchemaTypeLabel(*types);
  }
  auto allowed = schema.find("enum");
  if (allowed != schema.end() && allowed->is_array() &&
      std::find(allowed->begin(), allowed->end(), value) == allowed->end()) {
    return "`" + path + "` is not an allowed value";
  }

  if (value.is_number()) {
    const double number = value.get<double>();
    auto minimum = schema.find("minimum");
    if (minimum != schema.end() && minimum->is_number() &&
        number < minimum->get<double>()) {
      return "`" + path + "` is below its minimum";
    }
    auto maximum = schema.find("maximum");
    if (maximum != schema.end() && maximum->is_number() &&
        number > maximum->get<double>()) {
      return "`" + path + "` is above its maximum";
    }
  }
  if (value.is_string()) {
    const size_t size = value.get_ref<const std::string&>().size();
    if (std::optional<size_t> minimum = JsonSchemaSize(schema, "minLength");
        minimum && size < *minimum) {
      return "`" + path + "` is shorter than its minimum length";
    }
    if (std::optional<size_t> maximum = JsonSchemaSize(schema, "maxLength");
        maximum && size > *maximum) {
      return "`" + path + "` exceeds its maximum length";
    }
  }
  if (value.is_array()) return InvalidArrayValue(schema, value, path);
  if (value.is_object()) return InvalidObjectValue(schema, value, path, root);
  return "";
}

inline std::string InvalidToolArgument(const Tool& tool, const json& args) {
  return InvalidSchemaValue(ToolParameters(tool), args, tool.name,
                            /*root=*/true);
}

inline std::string StableArgumentError(
    const Tool& tool, const json& args,
    std::unordered_map<std::string, std::string>& values) {
  if (tool.stable_argument.empty()) return "";
  auto value = args.find(tool.stable_argument);
  if (value == args.end() || !value->is_string()) return "";
  std::string key = tool.name + "\n" + tool.stable_argument;
  auto [found, inserted] = values.emplace(key, value->get<std::string>());
  if (inserted || found->second == value->get_ref<const std::string&>()) {
    return "";
  }
  return "error: `" + tool.stable_argument + "` must remain `" + found->second +
         "` for this turn; reuse that artifact";
}

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

inline json AvailableToolSchemas(
    const std::vector<Tool>& tools, const json& schemas,
    const std::unordered_map<std::string, int64_t>& counts,
    ToolAvailability availability = {}) {
  json available = json::array();
  for (size_t i = 0; i < tools.size() && i < schemas.size(); ++i) {
    const Tool& tool = tools[i];
    if ((tool.visibility == Tool::Visibility::kCheckpointHint &&
         !availability.checkpoint_hint) ||
        (tool.visibility == Tool::Visibility::kDetachedTerminal &&
         !availability.detached_terminal)) {
      continue;
    }
    auto count = counts.find(tool.name);
    if (tool.max_calls_per_turn >= 0 && count != counts.end() &&
        count->second >= tool.max_calls_per_turn) {
      continue;
    }
    available.push_back(schemas[i]);
  }
  return available;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_TOOL_H_
