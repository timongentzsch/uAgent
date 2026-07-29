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
            return JsonString(annotation, "url") == source.url;
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

inline void PrintLatestTrace(const json& archive,
                             const std::vector<Tool>& tools) {
  auto trace =
      std::find_if(archive.rbegin(), archive.rend(), [](const json& segment) {
        return JsonValue(segment, "reason", "") == "trace_pruned";
      });
  if (trace == archive.rend()) {
    printf("%s· no completed tool trace%s\n", DIM(), RST());
    return;
  }
  for (const json& message : JsonValue(*trace, "messages", json::array())) {
    std::string role = JsonValue(message, "role", "");
    if (role == "assistant" && message.contains("tool_calls") &&
        message["tool_calls"].is_array()) {
      for (const json& call : message["tool_calls"]) {
        PrintToolCallSummary(call, tools);
      }
    } else if (role == "tool" && message.contains("content") &&
               message["content"].is_string()) {
      PrintToolResultText(message["content"].get<std::string>());
    } else if (role == "user" && message.contains("content") &&
               message["content"].is_string() &&
               message["content"].get_ref<const std::string&>().starts_with(
                   "[tool_result ")) {
      PrintToolResultText(message["content"].get<std::string>());
    }
  }
  PrintSearchReceipt(JsonValue(*trace, "web_searches", int64_t{0}),
                     JsonValue(*trace, "annotations", json::array()), true);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TRACE_H_
