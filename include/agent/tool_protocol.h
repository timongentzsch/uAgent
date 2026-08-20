// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TOOL_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_TOOL_PROTOCOL_H_

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace uagent {

inline constexpr const char* kTtOpen = "[uagent_tool_call]";
inline constexpr const char* kTtClose = "[/uagent_tool_call]";

enum class LeadingToolMarkup { kUndecided, kProse, kCall };

// Detection is intentionally broader than execution. Unknown provider markup
// must never become a second executable protocol, but holding it out of the
// answer stream prevents a malformed call from masquerading as user-facing
// prose while the turn loop asks the model to recover.
inline bool IsForeignToolCallOpener(std::string_view opener) {
  if (opener.empty() || opener.front() != '<') return false;
  std::string normalized;
  normalized.reserve(opener.size());
  for (char value : opener.substr(1)) {
    unsigned char byte = static_cast<unsigned char>(value);
    if (byte < 128 && std::isalnum(byte)) {
      normalized.push_back(static_cast<char>(std::tolower(byte)));
    }
  }
  size_t tool = normalized.find("tool");
  return tool != std::string::npos &&
         normalized.find("call", tool + 4) != std::string::npos;
}

// Classify only the leading non-whitespace bytes. Text-protocol calls must
// occupy the whole assistant message, and provider-emitted foreign protocols
// likewise start at the response boundary. `complete` resolves a final partial
// marker without making incremental SSE chunks flash on screen.
inline LeadingToolMarkup ClassifyLeadingToolMarkup(std::string_view content,
                                                   bool complete = false) {
  size_t start = content.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return complete ? LeadingToolMarkup::kProse : LeadingToolMarkup::kUndecided;
  }
  std::string_view visible = content.substr(start);
  std::string_view own(kTtOpen);
  if (visible.starts_with(own)) return LeadingToolMarkup::kCall;
  if (own.starts_with(visible)) {
    return complete ? LeadingToolMarkup::kProse : LeadingToolMarkup::kUndecided;
  }
  if (visible.front() != '<') return LeadingToolMarkup::kProse;

  size_t close = visible.find_first_of(">:\r\n", 1);
  if (close == std::string_view::npos) {
    if (!complete && visible.size() <= 257) {
      return LeadingToolMarkup::kUndecided;
    }
    close = std::min<size_t>(visible.size(), 257);
  }
  if (close > 256) return LeadingToolMarkup::kProse;
  return IsForeignToolCallOpener(visible.substr(0, close + 1))
             ? LeadingToolMarkup::kCall
             : LeadingToolMarkup::kProse;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TOOL_PROTOCOL_H_
