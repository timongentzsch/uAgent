// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TRACE_H_
#define UAGENT_INCLUDE_AGENT_TRACE_H_
// Shared state and terminal rendering for live, resumed, and archived traces.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/api/citations.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/tools/tool.h"

namespace uagent {

struct SearchTrace {
  static constexpr size_t kMaxSources = 20;
  static constexpr size_t kMaxTitleChars = 512;
  static constexpr size_t kMaxContentChars = 4096;

  int64_t requests = 0;
  json annotations = json::array();

  void Reset() {
    requests = 0;
    annotations = json::array();
  }

  void Add(int64_t count, const json& added) {
    requests += count;
    for (CitationEntry& source : CitationEntries(added)) {
      bool duplicate = std::any_of(
          annotations.begin(), annotations.end(), [&](const json& annotation) {
            return JsonValue(annotation, "url", "") == source.url;
          });
      if (duplicate || annotations.size() >= kMaxSources) continue;
      annotations.push_back(
          {{"url", std::move(source.url)},
           {"title", Utf8Trunc(std::move(source.title), kMaxTitleChars)},
           {"content",
            Utf8Trunc(std::move(source.content), kMaxContentChars)}});
    }
  }

  bool Empty() const { return requests == 0 && annotations.empty(); }

  json ArchiveMetadata() const {
    if (Empty()) return json::object();
    return {{"web_searches", requests}, {"annotations", annotations}};
  }
};

inline void PrintToolCallSummary(const json& call,
                                 const std::vector<Tool>& tools) {
  if (!call.is_object() || !call.contains("function") ||
      !call["function"].is_object()) {
    return;
  }
  const json& function = call["function"];
  std::string name = JsonValue(function, "name", "");
  json args =
      json::parse(JsonValue(function, "arguments", "{}"), nullptr, false);
  const Tool* tool = FindTool(tools, name);
  std::string summary = tool ? ToolSummary(*tool, args) : JsonDump(args);
  printf("%s→ %s(%s)%s\n", CYAN(), TerminalSafe(name).c_str(),
         TerminalSafe(FirstLine(summary)).c_str(), RST());
}

inline void PrintToolResultText(const std::string& result) {
  printf("%s  ← %s%s\n", DIM(), TerminalSafe(result).c_str(), RST());
}

inline bool PrintSearchReceipt(int64_t searches, const json& annotations,
                               bool details = false, bool line_open = false) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (searches <= 0 && sources.empty()) return false;
  if (line_open) printf("\n");
  std::string source_summary =
      sources.empty() ? "source details unavailable"
                      : std::to_string(sources.size()) + " source" +
                            (sources.size() == 1 ? "" : "s");
  if (searches > 0) {
    printf("%s  ← web_search ×%s · %s%s\n", DIM(),
           std::to_string(searches).c_str(), source_summary.c_str(), RST());
  } else {
    printf("%s  ← %s%s\n", DIM(), source_summary.c_str(), RST());
  }
  if (!details) return true;
  for (const CitationEntry& source : sources) {
    const std::string& label = source.title.empty() ? source.url : source.title;
    printf("%s    %s · %s%s\n", DIM(), TerminalSafe(label).c_str(),
           TerminalSafe(source.url).c_str(), RST());
    if (!source.content.empty()) {
      printf("%s      %s%s\n", DIM(), TerminalSafe(source.content).c_str(),
             RST());
    }
  }
  return true;
}

inline void PrintCitationSources(const json& annotations) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (sources.empty()) return;
  printf("\n%sSources:%s\n", DIM(), RST());
  for (const CitationEntry& source : sources) {
    printf("%s- <%s>%s\n", DIM(), TerminalSafe(source.url).c_str(), RST());
  }
}

inline json ToolTraceMessages(const json& messages, const json& kinds) {
  json trace = json::array();
  for (size_t index = 0; index < messages.size(); ++index) {
    const json& message = messages[index];
    if (!message.is_object()) continue;
    if (JsonValue(message, "role", "") == "assistant" &&
        message.contains("tool_calls") && message["tool_calls"].is_array()) {
      for (const json& call : message["tool_calls"]) {
        if (!call.is_object() || !call.contains("function") ||
            !call["function"].is_object()) {
          continue;
        }
        const json& function = call["function"];
        std::string raw = JsonValue(function, "arguments", "{}");
        json arguments = json::parse(raw, nullptr, false);
        if (arguments.is_discarded()) arguments = raw;
        trace.push_back({{"type", "function"},
                         {"id", JsonValue(call, "id", "")},
                         {"name", JsonValue(function, "name", "")},
                         {"arguments", std::move(arguments)},
                         {"result", nullptr}});
      }
      continue;
    }
    MessageKind kind = MessageKind::kInternal;
    if (index >= kinds.size() || !kinds[index].is_string() ||
        !ParseMessageKind(kinds[index].get<std::string>(), kind) ||
        kind != MessageKind::kToolResult) {
      continue;
    }
    std::string id = JsonValue(message, "tool_call_id", "");
    auto call = std::find_if(
        trace.rbegin(), trace.rend(),
        [&](const json& item) { return JsonValue(item, "id", "") == id; });
    if (call != trace.rend()) {
      (*call)["result"] = JsonValue(message, "content", "");
    }
  }
  return trace;
}

inline json LatestToolTraceJson(const json& archive) {
  auto segment =
      std::find_if(archive.rbegin(), archive.rend(), [](const json& item) {
        std::string reason = JsonValue(item, "reason", "");
        return reason == "tool_trace" || reason == "trace_pruned";
      });
  if (segment == archive.rend()) return json::array();
  json trace =
      ToolTraceMessages(JsonValue(*segment, "messages", json::array()),
                        JsonValue(*segment, "message_kinds", json::array()));
  int64_t searches = JsonValue(*segment, "web_searches", int64_t{0});
  if (searches > 0) {
    trace.push_back(
        {{"type", "server"},
         {"name", "web_search"},
         {"count", searches},
         {"sources", JsonValue(*segment, "annotations", json::array())}});
  }
  return trace;
}

inline void PrintLatestTrace(const json& archive,
                             const std::vector<Tool>& tools) {
  auto trace =
      std::find_if(archive.rbegin(), archive.rend(), [](const json& segment) {
        std::string reason = JsonValue(segment, "reason", "");
        return reason == "tool_trace" || reason == "trace_pruned";
      });
  if (trace == archive.rend()) {
    printf("%s· no completed tool trace%s\n", DIM(), RST());
    return;
  }
  const json& messages = JsonValue(*trace, "messages", json::array());
  const json& kinds = JsonValue(*trace, "message_kinds", json::array());
  for (size_t index = 0; index < messages.size(); ++index) {
    const json& message = messages[index];
    std::string role = JsonValue(message, "role", "");
    MessageKind kind = MessageKind::kInternal;
    if (index < kinds.size() && kinds[index].is_string()) {
      ParseMessageKind(kinds[index].get<std::string>(), kind);
    }
    if (role == "assistant" && message.contains("tool_calls") &&
        message["tool_calls"].is_array()) {
      for (const json& call : message["tool_calls"]) {
        PrintToolCallSummary(call, tools);
      }
    } else if (kind == MessageKind::kToolResult &&
               message.contains("content") && message["content"].is_string()) {
      PrintToolResultText(message["content"].get<std::string>());
    }
  }
  PrintSearchReceipt(JsonValue(*trace, "web_searches", int64_t{0}),
                     JsonValue(*trace, "annotations", json::array()), true);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TRACE_H_
