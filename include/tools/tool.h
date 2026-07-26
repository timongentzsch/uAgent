// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_TOOL_H_
#define UAGENT_INCLUDE_TOOLS_TOOL_H_
// The Tool type and registry machinery. Each Tool bundles its OpenAI
// schema with its handler, so adding a capability means appending to the
// registry; nothing in the agent loop changes.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

using nlohmann::json;

struct ToolContext {
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  int64_t timeout_s = 0;

  ToolContext WithTimeout(int64_t seconds) const {
    ToolContext out = *this;
    out.timeout_s = std::max(int64_t{0}, seconds);
    if (seconds > 0) {
      auto local =
          std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
      out.deadline = std::min(out.deadline, local);
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

struct Tool {
  using Run = std::function<std::string(const json&, const ToolContext&)>;
  using Summary = std::function<std::string(const json&)>;
  using Approval = std::function<bool(const json&)>;

  std::string name;
  std::string description;
  json parameters;        // JSON-schema for the args
  bool mutating = false;  // gated behind user approval
  Run run;
  Summary summary;                    // args -> one-line display
  bool parallel_safe = false;         // safe beside another tool call
  bool show_spinner = false;          // animate while a quiet call blocks
  Approval needs_approval;            // dynamic policy (e.g. external read)
  std::string provider;               // owner for live registry refresh
  json output_schema;                 // optional MCP output contract
  int64_t timeout_s = -1;             // -1 = global default; 0 = turn limit
  int64_t max_timeout_s = -1;         // -1 = uncapped; else clamps `timeout`
  int64_t result_chars = -1;          // -1 = global result cap
  int64_t max_calls_per_turn = -1;    // -1 = global turn budget
  bool full_terminal_output = false;  // show complete call + result
};

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

inline std::string ToolDescription(const Tool& tool) {
  std::string s = tool.description;
  // Mark tools that actually overlap, so the base prompt's batching rule is
  // actionable.
  if (tool.parallel_safe) s += " Batchable.";
  if (tool.max_calls_per_turn >= 0) {
    s += " Limit: " + std::to_string(tool.max_calls_per_turn) + "/turn.";
  }
  return s;
}

// One execution policy for built-ins, MCP, and future providers. Tool-specific
// schemas may refine the wording, but every registered tool exposes the same
// optional foreground timeout without repeating it at each registration site.
inline json ToolParameters(const Tool& tool, int64_t default_timeout_s = 30) {
  json parameters = tool.parameters;
  if (!parameters.is_object()) parameters = json::object();
  if (!parameters.contains("type")) parameters["type"] = "object";
  if (!parameters.contains("properties") ||
      !parameters["properties"].is_object()) {
    parameters["properties"] = json::object();
  }
  if (!parameters["properties"].contains("timeout")) {
    int64_t timeout = tool.timeout_s >= 0 ? tool.timeout_s : default_timeout_s;
    parameters["properties"]["timeout"] = {
        {"type", "integer"},
        {"minimum", 0},
        {"description", "foreground seconds; default " +
                            std::to_string(timeout) + "; 0=turn"}};
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

// first schema-required argument missing from args, or "" if all present —
// so a call like write_file without `content` errors instead of truncating
inline std::string MissingRequired(const Tool& t, const json& args) {
  if (t.parameters.contains("required")) {
    for (auto& r : t.parameters["required"]) {
      if (!r.is_string()) continue;
      const std::string& name = r.get_ref<const std::string&>();
      if (!args.contains(name)) return name;
    }
  }
  return "";
}

inline std::string InvalidArgumentType(const Tool& tool, const json& args) {
  json parameters = ToolParameters(tool);
  for (const auto& [name, schema] : parameters["properties"].items()) {
    if (!args.contains(name) || !schema.is_object() ||
        !schema.contains("type") || !schema["type"].is_string()) {
      continue;
    }
    const json& value = args[name];
    const std::string type = schema["type"].get<std::string>();
    bool valid = (type == "string" && value.is_string()) ||
                 (type == "integer" && value.is_number_integer()) ||
                 (type == "number" && value.is_number()) ||
                 (type == "boolean" && value.is_boolean()) ||
                 (type == "object" && value.is_object()) ||
                 (type == "array" && value.is_array()) ||
                 (type == "null" && value.is_null());
    if (!valid) return "`" + name + "` must be " + type;
    if (type == "integer" && schema.contains("minimum") &&
        schema["minimum"].is_number_integer() &&
        value.get<int64_t>() < schema["minimum"].get<int64_t>()) {
      return "`" + name + "` is below its minimum";
    }
  }
  return "";
}

// registry -> the `tools` array for a chat request
inline json ToolSchemas(const std::vector<Tool>& tools,
                        int64_t default_timeout_s = 30) {
  json out = json::array();
  for (auto& t : tools) {
    out.push_back({{"type", "function"},
                   {"function",
                    {{"name", t.name},
                     {"description", ToolDescription(t)},
                     {"parameters", ToolParameters(t, default_timeout_s)}}}});
  }
  return out;
}

inline json AvailableToolSchemas(
    const std::vector<Tool>& tools, const json& schemas,
    const std::unordered_map<std::string, int64_t>& counts) {
  json available = json::array();
  for (size_t i = 0; i < tools.size() && i < schemas.size(); ++i) {
    const Tool& tool = tools[i];
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
