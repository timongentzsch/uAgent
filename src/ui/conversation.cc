// Copyright 2026 Timon Gentzsch

#include "include/ui/conversation.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/agent/trace.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/media/terminal.h"
#include "include/tools/tool.h"
#include "include/ui/tool_output.h"

namespace uagent {

std::string ReplayableImagePath(const json& call, const std::string& result) {
  if (!result.starts_with("displayed ") || !call.is_object() ||
      !call.contains("function") || !call["function"].is_object()) {
    return "";
  }
  const json& function = call["function"];
  if (JsonValue(function, "name", "") != "show_image" ||
      !function.contains("arguments")) {
    return "";
  }
  json arguments;
  if (function["arguments"].is_object()) {
    arguments = function["arguments"];
  } else if (function["arguments"].is_string()) {
    arguments =
        json::parse(function["arguments"].get<std::string>(), nullptr, false);
  }
  if (!arguments.is_object()) return "";
  return JsonValue(arguments, "path", "");
}

void PrintConversationHistory(const Conversation& conversation,
                              const std::vector<Tool>& tools) {
  static const json kEmpty;
  std::unordered_map<std::string, std::string> tool_names;
  std::unordered_map<std::string, const json*> image_calls;
  for (size_t index = 0; index < conversation.Size(); ++index) {
    const json& message = conversation.At(index);
    MessageKind kind = conversation.KindAt(index);
    const json& content =
        message.contains("content") ? message["content"] : kEmpty;
    if (kind == MessageKind::kSystem) continue;
    if (kind == MessageKind::kToolResult && content.is_string()) {
      const std::string& stored = content.get_ref<const std::string&>();
      std::string text_name;
      std::string text_result;
      if (ParseTextToolResult(stored, text_name, text_result)) {
        PrintStoredToolResult(text_name, text_result);
        continue;
      }
      std::string id = JsonValue(message, "tool_call_id", "");
      auto name = tool_names.find(id);
      auto image = image_calls.find(id);
      if (image != image_calls.end()) {
        std::string path = ReplayableImagePath(*image->second, stored);
        if (!path.empty()) (void)ToolShowImage(path);
      }
      const std::string* display = conversation.ToolDisplay(id);
      PrintStoredToolResult(name == tool_names.end() ? "" : name->second,
                            stored, display ? *display : std::string());
    } else if (kind == MessageKind::kAssistant) {
      std::vector<ToolCall> text_calls;
      if (content.is_string()) {
        const std::string& text = content.get_ref<const std::string&>();
        text_calls = ParseTextToolCalls(text);
        if (!text.empty() && text_calls.empty()) {
          MdPrint(text);
          printf("\n");
        }
      }
      if (message.contains("tool_calls")) {
        for (const json& call : message["tool_calls"]) {
          std::string name = PrintToolCallSummary(call, tools);
          std::string id = JsonValue(call, "id", "");
          if (!id.empty() && !name.empty()) {
            const Tool* tool = FindTool(tools, name);
            if (tool && tool->replay_image) image_calls[id] = &call;
            tool_names[id] = std::move(name);
          }
        }
      } else {
        for (const ToolCall& call : text_calls) {
          json stored_call = {
              {"id", call.id},
              {"function", {{"name", call.name}, {"arguments", call.args}}}};
          PrintToolCallSummary(stored_call, tools);
        }
      }
    } else if (kind == MessageKind::kUser && content.is_string()) {
      std::string safe = TerminalSafe(content.get_ref<const std::string&>());
      printf("%s>%s %s\n", CYAN(), RST(), safe.c_str());
    } else if (kind == MessageKind::kAttachment) {
      printf("%s>%s [attachment]\n", CYAN(), RST());
    } else if (content.is_string()) {
      printf("%s  ← %s%s\n", DIM(),
             TerminalSafe(FirstLine(content.get_ref<const std::string&>()))
                 .c_str(),
             RST());
    }
  }
}

void PrintModelContext(const json& request) {
  printf("%s\n", TerminalSafe(JsonDump(request, 2)).c_str());
}

}  // namespace uagent
