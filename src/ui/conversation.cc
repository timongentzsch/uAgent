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
#include "include/tools/tool.h"
#include "include/ui/tool_output.h"

namespace uagent {

void PrintConversationHistory(const Conversation& conversation,
                              const std::vector<Tool>& tools) {
  static const json kEmpty;
  std::unordered_map<std::string, std::string> tool_names;
  for (size_t index = 0; index < conversation.Size(); ++index) {
    const json& message = conversation.At(index);
    MessageKind kind = conversation.KindAt(index);
    const json& content =
        message.contains("content") ? message["content"] : kEmpty;
    if (kind == MessageKind::kSystem) continue;
    if (kind == MessageKind::kToolResult && content.is_string()) {
      std::string id = JsonValue(message, "tool_call_id", "");
      auto name = tool_names.find(id);
      PrintStoredToolResult(name == tool_names.end() ? "" : name->second,
                            content.get<std::string>());
    } else if (kind == MessageKind::kAssistant) {
      if (content.is_string() && !content.get<std::string>().empty()) {
        MdPrint(content.get<std::string>());
        printf("\n");
      }
      if (message.contains("tool_calls")) {
        for (const json& call : message["tool_calls"]) {
          std::string name = PrintToolCallSummary(call, tools);
          std::string id = JsonValue(call, "id", "");
          if (!id.empty() && !name.empty()) tool_names[id] = std::move(name);
        }
      }
    } else if (kind == MessageKind::kUser && content.is_string()) {
      std::string safe = TerminalSafe(content.get<std::string>());
      printf("%s>%s %s\n", CYAN(), RST(), safe.c_str());
    } else if (kind == MessageKind::kAttachment) {
      printf("%s>%s [attachment]\n", CYAN(), RST());
    } else if (content.is_string()) {
      printf("%s  ← %s%s\n", DIM(),
             TerminalSafe(FirstLine(content.get<std::string>())).c_str(),
             RST());
    }
  }
}

void PrintModelContext(const json& request) {
  printf("%s\n", TerminalSafe(JsonDump(request, 2)).c_str());
}

}  // namespace uagent
