// Copyright 2026 Timon Gentzsch
#include <string>
#include <utility>

#include "include/agent/prompt.h"
#include "include/core/limits.h"

namespace uagent {
json ResolvePrompt(const std::string& base, const AdaptiveSystemState* self,
                   const json& context) {
  std::string text = base;
  json sources = json::array(
      {{{"scope", "built-in"}, {"text", base}, {"active", true}}});
  // The agent's own self-directive (adapt_system) overlays the base or, with
  // approval, replaces it; it never outlives the conversation.
  if (self && !self->instructions.empty()) {
    const bool replace = self->mode == "replace";
    if (replace) {
      text = self->instructions;
      sources[0]["active"] = false;
    } else {
      text += "\n\n" + self->instructions;
    }
    sources.push_back({{"scope", "conversation"},
                       {"mode", self->mode},
                       {"text", self->instructions},
                       {"revision", std::to_string(self->revision)},
                       {"active", true}});
  }
  for (auto source : context) {
    const auto body = JsonValue(source, "text", "");
    if (body.empty()) continue;
    text += "\n\n" + body;
    source["active"] = true;
    sources.push_back(std::move(source));
  }
  if (text.size() > kAdaptiveSystemBytes) {
    return {{"error",
             "The resolved system prompt exceeds 64 KiB. Shorten the "
             "instructions or the self-directive."}};
  }
  return {{"effective", text},
          {"sources", sources},
          {"digest", HashHex(text)},
          {"bytes", text.size()}};
}
}  // namespace uagent
