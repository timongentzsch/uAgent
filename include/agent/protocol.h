// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_PROTOCOL_H_
// The text-protocol fallback used when a server rejects native `tools`,
// the system prompt, and the predicates classifying a stored message.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "include/api.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

// --- text-protocol fallback -------------------------------------------------
// For servers without native tool-calling the model emits standalone
// [uagent_tool_call]{...}[/uagent_tool_call] blocks. Only a message that is
// ENTIRELY tool-call blocks is treated as calls — quoted examples inside
// prose or code blocks stay text.

inline std::vector<ToolCall> ParseTextToolCalls(const std::string& content) {
  std::vector<ToolCall> calls;
  std::string s = Trim(content);
  int idx = 0;
  while (!s.empty()) {
    if (!s.starts_with(kTtOpen)) {
      return {};  // leading prose -> not a call message
    }
    size_t close = s.find(kTtClose);
    if (close == std::string::npos) return {};
    std::string inner =
        Trim(s.substr(strlen(kTtOpen), close - strlen(kTtOpen)));
    json j = json::parse(inner, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("name") ||
        !j["name"].is_string()) {
      return {};
    }
    // some models emit `arguments` as a stringified object — pass it through
    json a = JsonValue(j, "arguments", json::object());
    calls.push_back({"text-" + std::to_string(idx++),
                     j["name"].get<std::string>(),
                     a.is_string() ? a.get<std::string>() : JsonDump(a)});
    s = Trim(s.substr(close + strlen(kTtClose)));
  }
  return calls;
}

// escape the delimiters so tool output can never fake a tool call
inline std::string EscapeToolTags(std::string s) {
  ReplaceAll(s, kTtOpen, "&#91;uagent_tool_call&#93;");
  ReplaceAll(s, kTtClose, "&#91;/uagent_tool_call&#93;");
  return s;
}

// cap huge results, keeping head + tail (errors usually live at the end)
inline std::string CapResult(std::string s, int64_t cap = -1) {
  if (cap < 0) cap = ToolResultCap();
  if (cap <= 0 || static_cast<int64_t>(s.size()) <= cap) return s;
  size_t half = static_cast<size_t>(cap) / 2;
  size_t head_end = Utf8BoundaryBefore(s, half);
  size_t tail_start = Utf8BoundaryAfter(s, s.size() - half);
  return s.substr(0, head_end) + "\n... [" +
         std::to_string(tail_start - head_end) + " bytes truncated] ...\n" +
         s.substr(tail_start);
}

// --- the agent ---------------------------------------------------------------

// Lean base prompt — tool semantics live in the tool schemas, which are sent
// anyway. The text protocol (plus a tool list, since schemas are no longer
// sent) is appended only after a server rejects native tool calls.
inline constexpr const char* kSystemPrompt =
    "You are a coding agent in the current directory. Resolve the request "
    "completely: inspect, edit, and verify with tools; do not guess. Keep "
    "changes minimal and focused, preserve unrelated work, and run relevant "
    "checks before claiming success. Commit or push only when asked. Batch "
    "independent calls and delegate independent subtasks. Direct instructions "
    "win; nearer AGENTS.md or CLAUDE.md overrides broader guidance—read "
    "applicable files before entering subtrees. Ask only when blocked. Prefer "
    "Unicode math; use LaTeX only when raw source is requested. Be concise and "
    "report blockers.";

inline std::string TextProtocolPrompt(const std::vector<Tool>& tools,
                                      int64_t default_timeout_s = 30) {
  std::string s =
      "\n\nNative tools unavailable. Reply only with one tool block per "
      "independent call, then wait:\n"
      "[uagent_tool_call]{\"name\": \"read_file\", \"arguments\": {\"path\": "
      "\"foo.py\"}}"
      "[/uagent_tool_call]\n"
      "Tools (? optional):\n";
  for (auto& t : tools) {
    json parameters = ToolParameters(t, default_timeout_s);
    auto required = [&](const std::string& k) {
      if (parameters.contains("required")) {
        for (auto& r : parameters["required"]) {
          if (r == k) return true;
        }
      }
      return false;
    };
    std::string args;
    if (parameters.contains("properties")) {
      for (int pass = 0; pass < 2; pass++) {  // required params first
        for (auto& [k, v] : parameters["properties"].items()) {
          if (required(k) == (pass == 0)) {
            if (!args.empty()) args += ", ";
            args += k;
            if (pass) args += "?";
          }
        }
      }
    }
    s += t.name + "(" + args + ")\n";
  }
  return s;
}

inline bool InternalUserText(const std::string& text) {
  static constexpr const char* kPrefixes[] = {
      "[now ",
      "[tool_result ",
      "[Background result:",
      "[context checkpoint ",
      "[checkpoint",
      "# AGENTS.md instructions for ",
      "Prior context:",
      "(response interrupted; partial output was discarded)",
  };
  for (const char* prefix : kPrefixes) {
    if (text.starts_with(prefix)) return true;
  }
  return false;
}

inline bool InternalAssistantText(const std::string& text) {
  return text.starts_with("[checkpoint ");
}

inline bool SecretCheckpointPath(const std::filesystem::path& path) {
  std::string name = path.filename().string();
  if (name == ".env" || name.starts_with(".env.")) return true;
  for (const std::string& config :
       {UagentConfigPath(), ProjectConfigFilePath()}) {
    if (!config.empty() && CanonicalAccessPath(config) == path) return true;
  }
  return false;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROTOCOL_H_
