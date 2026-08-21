// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TRACE_H_
#define UAGENT_INCLUDE_AGENT_TRACE_H_
// Shared state and terminal rendering for live, resumed, and archived traces.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/conversation.h"
#include "include/agent/protocol.h"
#include "include/api/citations.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/tools/tool.h"
#include "include/ui/presentation.h"

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

inline json ParsedToolCallArguments(const json& function) {
  if (!function.is_object() || !function.contains("arguments")) {
    return json::object();
  }
  if (!function["arguments"].is_string()) return function["arguments"];
  std::string raw = function["arguments"].get<std::string>();
  json arguments = json::parse(raw, nullptr, false);
  return arguments.is_discarded() ? json(std::move(raw)) : std::move(arguments);
}

inline PresentationRecord StoredToolCallPresentation(
    const std::string& name, const json& arguments,
    const std::vector<Tool>& tools, const std::string& ordinal = "") {
  PresentationRecord record;
  record.kind = PresentationKind::kToolCall;
  record.title = ordinal + name;
  const Tool* tool = FindTool(tools, name);
  SetCallLabel(record,
               tool && arguments.is_object()
                   ? ToolSummary(*tool, arguments)
                   : (arguments.is_string() ? arguments.get<std::string>()
                                            : JsonDump(arguments)));
  return record;
}

inline std::string PrintToolCallSummary(const json& call,
                                        const std::vector<Tool>& tools) {
  if (!call.is_object() || !call.contains("function") ||
      !call["function"].is_object()) {
    return "";
  }
  const json& function = call["function"];
  std::string name = JsonValue(function, "name", "");
  json args = ParsedToolCallArguments(function);
  PrintPresentation(StoredToolCallPresentation(name, args, tools));
  return name;
}

// Terminal rendering and message-to-trace projection live in
// src/agent/trace.cc: three call sites need them, every includer of
// agent.h was compiling them.
bool PrintSearchReceipt(int64_t searches, const json& annotations,
                        bool details = false, bool line_open = false);

void PrintCitationSources(const json& annotations);

// conversation messages + their kinds -> the call/result trace array
json ToolTraceMessages(const json& messages, const json& kinds);

void PrintTraceToolCall(const json& call, const std::vector<Tool>& tools,
                        const std::string& ordinal);

void PrintTraceToolResult(const json& call, const std::string& ordinal);

void PrintLatestTrace(const json& archive, const std::vector<Tool>& tools);

inline const json* LatestTraceSegment(const json& archive) {
  auto segment =
      std::find_if(archive.rbegin(), archive.rend(), [](const json& item) {
        std::string reason = JsonValue(item, "reason", "");
        // "trace_pruned" is what earlier releases wrote; their format-3
        // sessions still resume, so /trace must keep reading it.
        return reason == "tool_trace" || reason == "trace_pruned";
      });
  return segment == archive.rend() ? nullptr : &*segment;
}

inline json LatestToolTraceJson(const json& archive) {
  const json* segment = LatestTraceSegment(archive);
  if (!segment) return json::array();
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

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TRACE_H_
