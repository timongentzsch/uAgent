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

namespace uagent {

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

}  // namespace uagent
