// Copyright 2026 Timon Gentzsch
#include <string>
#include <utility>

#include "include/agent/prompt.h"
#include "include/core/limits.h"

namespace uagent {
json ResolvePrompt(const std::string& base, const json& context) {
  std::string text = base;
  json sources =
      json::array({{{"scope", "built-in"}, {"text", base}, {"active", true}}});
  for (auto source : context) {
    const auto body = JsonValue(source, "text", "");
    if (body.empty()) continue;
    text += "\n\n" + body;
    source["active"] = true;
    sources.push_back(std::move(source));
  }
  if (text.size() > kSystemPromptBytes) {
    return {{"error", "The resolved system prompt exceeds " +
                          std::to_string(kSystemPromptBytes / 1024) +
                          " KiB. Shorten the instructions."}};
  }
  return {{"effective", text},
          {"sources", sources},
          {"digest", HashHex(text)},
          {"bytes", text.size()}};
}
}  // namespace uagent
