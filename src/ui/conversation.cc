// Copyright 2026 Timon Gentzsch

#include "include/ui/conversation.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "include/agent/trace.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/tools/tool.h"

namespace uagent {

void PrintConversationHistory(const Conversation& conversation,
                              const std::vector<Tool>& tools) {
  static const json kEmpty;
  for (size_t index = 0; index < conversation.Size(); ++index) {
    const json& message = conversation.At(index);
    MessageKind kind = conversation.KindAt(index);
    const std::string role = JsonValue(message, "role", "");
    const json& content =
        message.contains("content") ? message["content"] : kEmpty;
    if (kind == MessageKind::kSystem) continue;
    if (kind == MessageKind::kToolResult && content.is_string()) {
      PrintToolResultText(content.get<std::string>());
    } else if (kind == MessageKind::kAssistant) {
      if (content.is_string() && !content.get<std::string>().empty()) {
        MdPrint(content.get<std::string>());
        printf("\n");
      }
      if (message.contains("tool_calls")) {
        for (const json& call : message["tool_calls"]) {
          PrintToolCallSummary(call, tools);
        }
      }
    } else if (kind == MessageKind::kUser && content.is_string()) {
      std::string safe = TerminalSafe(content.get<std::string>());
      printf("%s> %s%s\n", BOLD(), safe.c_str(), RST());
    } else if (kind == MessageKind::kAttachment) {
      printf("%s> [attachment]%s\n", BOLD(), RST());
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
