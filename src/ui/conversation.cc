// Copyright 2026 Timon Gentzsch

#include "include/ui/conversation.h"

#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/trace.h"
#include "include/cli.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/tools/tool.h"
#include "include/ui/presentation.h"
#include "include/ui/tool_output.h"

namespace uagent {

void PrintConversationHistory(const Conversation& conversation,
                              const std::vector<Tool>& tools) {
  static const json kEmpty;
  std::unordered_map<std::string, std::string> tool_names;
  std::map<uint64_t, json> summaries;
  for (const json& facts : conversation.DisplayFacts()) {
    if ((JsonValue(facts, "kind", "") == "turn_summary" ||
         JsonValue(facts, "kind", "") == "activity" ||
         JsonValue(facts, "kind", "") == "compaction") &&
        JsonValue(facts, "sequence", uint64_t{0}) >
            (conversation.DisplayIds().size() > 1 ? conversation.DisplayIds()[1]
                                                  : 0)) {
      summaries[JsonValue(facts, "sequence", uint64_t{0})] = facts;
    }
  }
  auto footer = [&](uint64_t before) {
    while (!summaries.empty() && summaries.begin()->first < before) {
      const json& entry = summaries.begin()->second;
      std::string text = JsonValue(entry, "kind", "") == "compaction"
                             ? "Context compacted"
                         : entry.contains("summary")
                             ? TurnStatsLine(entry["summary"])
                             : TerminalSafe(JsonValue(entry, "text", ""));
      printf("%s%s%s\n", DIM(), text.c_str(), RST());
      summaries.erase(summaries.begin());
    }
  };
  for (size_t index = 0; index < conversation.Size(); ++index) {
    if (index < conversation.DisplayIds().size()) {
      footer(conversation.DisplayIds()[index]);
    }
    const json& message = conversation.At(index);
    MessageKind kind = conversation.KindAt(index);
    const json& content =
        message.contains("content") ? message["content"] : kEmpty;
    if (kind == MessageKind::kSystem) continue;
    if (kind == MessageKind::kToolResult && content.is_string()) {
      const std::string& stored = content.get_ref<const std::string&>();
      std::string id = JsonValue(message, "tool_call_id", "");
      auto name = tool_names.find(id);
      const std::string* display = conversation.ToolDisplay(id);
      const json detail = JsonValue(conversation.DisplayFacts(),
                                    ("t-" + id).c_str(), json::object());
      const std::string status = JsonValue(detail, "status", "");
      const PresentationStatus presentation_status =
          status == "success"     ? PresentationStatus::kSucceeded
          : status == "cancelled" ? PresentationStatus::kCancelled
          : status == "failed" || status == "timed_out"
              ? PresentationStatus::kFailed
              : PresentationStatus::kNeutral;
      auto record = StoredToolResultPresentation(
          name == tool_names.end() ? "" : name->second, stored,
          display ? *display : std::string(), presentation_status);
      record.id = id;
      record.activity = JsonValue(detail, "activity", json::object());
      PrintPresentation(record);
    } else if (kind == MessageKind::kAssistant) {
      const bool has_text = content.is_string() &&
                            !content.get_ref<const std::string&>().empty();
      // Mirror the live presenter, which prints the mark lazily with the
      // first text: a text-empty assistant turn (tool calls only) shows
      // tool rows with no header, never a bare mark line.
      if (has_text) {
        PrintMessageHeader();
        MdPrint(content.get_ref<const std::string&>());
        printf("\n");
      }
      if (message.contains("tool_calls")) {
        for (const json& call : message["tool_calls"]) {
          std::string id = JsonValue(call, "id", "");
          const json activity =
              JsonValue(JsonValue(conversation.DisplayFacts(),
                                  ("t-" + id).c_str(), json::object()),
                        "activity", json::object());
          const json function = JsonValue(call, "function", json::object());
          std::string name = activity.contains("group")
                                 ? JsonValue(function, "name", "")
                                 : PrintToolCallSummary(call, tools);
          if (!id.empty() && !name.empty()) {
            tool_names[id] = std::move(name);
          }
        }
      }
    } else if (kind == MessageKind::kUser && content.is_string()) {
      std::string safe = TerminalSafe(content.get_ref<const std::string&>());
      printf("%s\n", UserEchoRow(InputPrompt(), safe).c_str());
    } else if ((kind == MessageKind::kAttachment ||
                kind == MessageKind::kUser) &&
               content.is_array()) {
      printf("%s\n",
             UserEchoRow(InputPrompt(),
                         content.is_array() && !content.empty()
                             ? JsonValue(content[0], "text", "[attachment]")
                             : "[attachment]")
                 .c_str());
    } else if (content.is_string()) {
      printf("%s  ← %s%s\n", DIM(),
             TerminalSafe(FirstLine(content.get_ref<const std::string&>()))
                 .c_str(),
             RST());
    }
  }
  footer(UINT64_MAX);
}

void PrintModelContext(const json& request) {
  printf("%s\n", TerminalSafe(JsonDump(request, 2)).c_str());
}

std::string PrintToolCallSummary(const json& call,
                                 const std::vector<Tool>& tools) {
  const json* found = JsonObject(call, "function");
  if (!found) return "";
  const json& function = *found;
  std::string name = JsonValue(function, "name", "");
  json args = ParsedToolCallArguments(function);
  PrintPresentation(ToolCallPresentation(name, args, tools));
  return name;
}

void PrintTraceToolCall(const json& call, const std::vector<Tool>& tools,
                        const std::string& ordinal) {
  std::string name = JsonValue(call, "name", "tool");
  json arguments =
      call.contains("arguments") ? call["arguments"] : json::object();
  PrintPresentation(ToolCallPresentation(name, arguments, tools, ordinal));
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
