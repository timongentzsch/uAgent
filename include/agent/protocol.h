// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_PROTOCOL_H_
// The text-protocol fallback used when a server rejects native `tools`:
// parsing, detection and result shaping. Prompt authoring lives in prompt.h.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/tool_protocol.h"
#include "include/api/types.h"
#include "include/core/env.h"
#include "include/core/json.h"
#include "include/core/strings.h"

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

inline bool ParseTextToolResult(const std::string& content, std::string& name,
                                std::string& result) {
  constexpr std::string_view kPrefix = "[tool_result ";
  if (!content.starts_with(kPrefix)) return false;
  size_t close = content.find("]\n", kPrefix.size());
  if (close == std::string::npos) return false;
  name = content.substr(kPrefix.size(), close - kPrefix.size());
  if (name.empty()) return false;
  result = content.substr(close + 2);
  return true;
}

// Reject any structured marker whose normalized opener contains `tool`
// followed by `call`. This is detection-only and deliberately provider-
// agnostic: it recognizes unseen delimiter variants without turning them into
// executable syntax. Markdown code examples are ignored.
inline bool ContainsForeignToolCallMarkup(const std::string& content) {
  bool fenced = false;
  bool inline_code = false;
  bool line_start = true;
  for (size_t index = 0; index < content.size();) {
    if (line_start) {
      size_t first = content.find_first_not_of(" \t", index);
      if (first == std::string::npos) return false;
      if (content.compare(first, 3, "```") == 0 ||
          content.compare(first, 3, "~~~") == 0) {
        fenced = !fenced;
      }
      line_start = false;
    }
    char current = content[index];
    if (current == '\n' || current == '\r') {
      line_start = true;
      inline_code = false;
      ++index;
      continue;
    }
    if (fenced) {
      ++index;
      continue;
    }
    if (current == '`') {
      inline_code = !inline_code;
      ++index;
      continue;
    }
    if (inline_code || current != '<') {
      ++index;
      continue;
    }
    size_t close = content.find_first_of(">:\r\n", index + 1);
    if (close == std::string::npos || close - index > 256) {
      ++index;
      continue;
    }
    if (IsForeignToolCallOpener(
            std::string_view(content).substr(index, close - index + 1))) {
      return true;
    }
    index = close + 1;
  }
  return false;
}

// A response that is usable as evidence rather than as a step: real text, and
// no tool call in any protocol. Required of the summarizer and of the vision
// model, neither of which has tools.
inline bool ProseOnlyResponse(const ChatResult& result) {
  return !result.content.empty() && result.tool_calls.empty() &&
         ParseTextToolCalls(result.content).empty() &&
         !ContainsForeignToolCallMarkup(result.content);
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
  size_t limit = static_cast<size_t>(cap);
  if (limit <= 3) return std::string(limit, '.');

  size_t omitted = s.size() - limit;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::string marker =
        "\n... [" + std::to_string(omitted) + " bytes truncated] ...\n";
    if (marker.size() >= limit) {
      return Utf8Prefix(std::move(s), limit - 3) + "...";
    }
    size_t keep = limit - marker.size();
    size_t head_end = Utf8BoundaryBefore(s, keep / 2);
    size_t tail_start = Utf8BoundaryAfter(s, s.size() - (keep - keep / 2));
    if (tail_start < head_end) tail_start = head_end;
    size_t actual_omitted = tail_start - head_end;
    if (actual_omitted == omitted) {
      return s.substr(0, head_end) + marker + s.substr(tail_start);
    }
    omitted = actual_omitted;
  }
  return Utf8Prefix(std::move(s), limit - 3) + "...";
}

inline json HarnessMessage(std::string content) {
  return {{"role", "system"}, {"content", std::move(content)}};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PROTOCOL_H_
