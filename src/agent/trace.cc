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
  std::vector<size_t> pending_text_calls;
  auto append_call = [&](const ToolCall& call, bool text_protocol) {
    json arguments = json::parse(call.args, nullptr, false);
    if (arguments.is_discarded()) arguments = call.args;
    json item = {{"type", "function"},
                 {"id", call.id},
                 {"name", call.name},
                 {"arguments", std::move(arguments)},
                 {"result", nullptr}};
    if (text_protocol) item["text_protocol"] = true;
    trace.push_back(std::move(item));
    if (text_protocol) pending_text_calls.push_back(trace.size() - 1);
  };

  for (size_t index = 0; index < messages.size(); ++index) {
    const json& message = messages[index];
    if (!message.is_object()) continue;
    if (JsonValue(message, "role", "") == "assistant") {
      if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const json& call : message["tool_calls"]) {
          if (!call.is_object() || !call.contains("function") ||
              !call["function"].is_object()) {
            continue;
          }
          const json& function = call["function"];
          json arguments = ParsedToolCallArguments(function);
          append_call(
              {JsonValue(call, "id", ""), JsonValue(function, "name", ""),
               arguments.is_string() ? arguments.get<std::string>()
                                     : JsonDump(arguments)},
              /*text_protocol=*/false);
        }
      } else if (message.contains("content") &&
                 message["content"].is_string()) {
        for (const ToolCall& call :
             ParseTextToolCalls(message["content"].get<std::string>())) {
          append_call(call, /*text_protocol=*/true);
        }
      }
      continue;
    }
    MessageKind kind = MessageKind::kInternal;
    if (index >= kinds.size() || !kinds[index].is_string() ||
        !ParseMessageKind(kinds[index].get<std::string>(), kind) ||
        kind != MessageKind::kToolResult) {
      continue;
    }
    std::string content = JsonValue(message, "content", "");
    std::string id = JsonValue(message, "tool_call_id", "");
    if (!id.empty()) {
      auto call =
          std::find_if(trace.rbegin(), trace.rend(), [&](const json& item) {
            return JsonValue(item, "id", "") == id && item["result"].is_null();
          });
      if (call != trace.rend()) (*call)["result"] = std::move(content);
      continue;
    }
    std::string name;
    std::string result;
    if (!ParseTextToolResult(content, name, result)) continue;
    for (size_t call_index : pending_text_calls) {
      json& call = trace[call_index];
      if (call["result"].is_null() && JsonValue(call, "name", "") == name) {
        call["result"] = std::move(result);
        break;
      }
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
