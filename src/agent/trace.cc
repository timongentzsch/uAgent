// Copyright 2026 Timon Gentzsch

#include "include/agent/trace.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

bool PrintSearchReceipt(int64_t searches, const json& annotations, bool details,
                        bool line_open) {
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

void PrintCitationSources(const json& annotations) {
  std::vector<CitationEntry> sources = CitationEntries(annotations);
  if (sources.empty()) return;
  printf("\n%sSources:%s\n", DIM(), RST());
  for (const CitationEntry& source : sources) {
    printf("%s- <%s>%s\n", DIM(), TerminalSafe(source.url).c_str(), RST());
  }
}

json ToolTraceMessages(const json& messages, const json& kinds) {
  json trace = json::array();
  for (size_t index = 0; index < messages.size(); ++index) {
    const json& message = messages[index];
    if (!message.is_object()) continue;
    if (JsonValue(message, "role", "") == "assistant") {
      const json* tool_calls = JsonArray(message, "tool_calls");
      if (tool_calls == nullptr) continue;
      for (const json& call : *tool_calls) {
        const json* found = JsonObject(call, "function");
        if (!found) continue;
        json arguments = ParsedToolCallArguments(*found);
        json stored =
            json::parse(arguments.is_string() ? arguments.get<std::string>()
                                              : JsonDump(arguments),
                        nullptr, false);
        trace.push_back(
            {{"type", "function"},
             {"id", JsonValue(call, "id", "")},
             {"name", JsonValue(*found, "name", "")},
             {"arguments",
              stored.is_discarded() ? std::move(arguments) : std::move(stored)},
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
    if (id.empty()) continue;
    auto call =
        std::find_if(trace.rbegin(), trace.rend(), [&](const json& item) {
          return JsonValue(item, "id", "") == id && item["result"].is_null();
        });
    if (call != trace.rend()) {
      (*call)["result"] = JsonValue(message, "content", "");
    }
  }
  return trace;
}

void PrintTraceToolCall(const json& call, const std::vector<Tool>& tools,
                        const std::string& ordinal) {
  std::string name = JsonValue(call, "name", "tool");
  json arguments =
      call.contains("arguments") ? call["arguments"] : json::object();
  PrintPresentation(
      StoredToolCallPresentation(name, arguments, tools, ordinal));
}

void PrintTraceToolResult(const json& call, const std::string& ordinal) {
  std::string name = JsonValue(call, "name", "tool");
  PresentationRecord record;
  record.kind = PresentationKind::kToolResult;
  record.title = ordinal + name;
  record.status = PresentationStatus::kSucceeded;
  if (!call.contains("result") || call["result"].is_null()) {
    record.summary = "(no result)";
    PrintPresentation(record);
    return;
  }
  std::string result = call["result"].is_string()
                           ? call["result"].get<std::string>()
                           : JsonDump(call["result"]);
  if (result.find('\n') != std::string::npos) {
    record.detail = std::move(result);
    record.multiline = true;
  } else {
    record.summary = std::move(result);
  }
  PrintPresentation(record);
}

void PrintLatestTrace(const json& archive, const std::vector<Tool>& tools) {
  const json* segment = LatestTraceSegment(archive);
  if (!segment) {
    printf("%s· no completed tool trace%s\n", DIM(), RST());
    return;
  }
  json calls =
      ToolTraceMessages(JsonValue(*segment, "messages", json::array()),
                        JsonValue(*segment, "message_kinds", json::array()));
  size_t tool_count = calls.size();
  int64_t searches = JsonValue(*segment, "web_searches", int64_t{0});
  std::string turn = std::to_string(JsonValue(*segment, "turn", int64_t{0}));
  printf("%slatest trace · turn %s · %zu tool%s", DIM(), turn.c_str(),
         tool_count, tool_count == 1 ? "" : "s");
  if (searches > 0) {
    std::string count = std::to_string(searches);
    printf(" · %s search%s", count.c_str(), searches == 1 ? "" : "es");
  }
  printf("%s\n", RST());
  for (size_t index = 0; index < calls.size(); ++index) {
    std::string ordinal =
        calls.size() > 1 ? "[" + std::to_string(index + 1) + "] " : "";
    PrintTraceToolCall(calls[index], tools, ordinal);
    PrintTraceToolResult(calls[index], ordinal);
  }
  PrintSearchReceipt(searches,
                     JsonValue(*segment, "annotations", json::array()), true);
}

}  // namespace uagent
