// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_TOOL_PROTOCOL_H_
#define UAGENT_INCLUDE_AGENT_TOOL_PROTOCOL_H_
// Recognizing tool-call markup this harness does not execute. µAgent runs
// structured provider tool calls only; a model that emits some other
// provider's call syntax as content is producing a malformed response, and
// this is how that is detected so the turn loop can suppress it and recover
// rather than print it as an answer. Detection is deliberately broader than
// execution, and nothing here turns markup into a call.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace uagent {

enum class LeadingToolMarkup { kUndecided, kProse, kCall };

inline bool IsForeignToolCallOpener(std::string_view opener) {
  if (opener.empty() || (opener.front() != '<' && opener.front() != '[')) {
    return false;
  }
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

// A `[...]` marker counts only when arguments follow it immediately. Prose
// says "[tool call] failed" and means it; a model emitting call syntax always
// puts its JSON straight after the marker. An angle-bracket span needs no such
// guard because `<...tool...call...>` is already markup.
inline bool BracketedToolCallMarker(std::string_view text, size_t open) {
  size_t close = text.find(']', open + 1);
  if (close == std::string_view::npos || close - open > 256) return false;
  if (close + 1 >= text.size() || text[close + 1] != '{') return false;
  return IsForeignToolCallOpener(text.substr(open, close - open + 1));
}

// Classify only the leading non-whitespace bytes: a provider-emitted foreign
// protocol starts at the response boundary. `complete` resolves a final
// partial marker without making incremental SSE chunks flash on screen.
inline LeadingToolMarkup ClassifyLeadingToolMarkup(std::string_view content,
                                                   bool complete = false) {
  size_t start = content.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return complete ? LeadingToolMarkup::kProse : LeadingToolMarkup::kUndecided;
  }
  std::string_view visible = content.substr(start);
  if (visible.front() == '[') {
    // Undecided until the arguments arrive: the guard needs the byte after
    // the closing bracket, and a stream can stop anywhere.
    if (BracketedToolCallMarker(visible, 0)) return LeadingToolMarkup::kCall;
    size_t close = visible.find(']');
    if (!complete &&
        (close == std::string_view::npos || close + 1 >= visible.size()) &&
        visible.size() <= 257) {
      return LeadingToolMarkup::kUndecided;
    }
    return LeadingToolMarkup::kProse;
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

// The same question asked of a whole message rather than its opening bytes.
// Markdown code examples are ignored, so a page documenting `<tool_call>`
// stays prose.
inline bool ContainsForeignToolCallMarkup(const std::string& content) {
  bool fenced = false;
  bool inline_code = false;
  bool line_start = true;
  for (size_t index = 0; index < content.size();) {
    if (line_start) {
      size_t first = content.find_first_not_of(" \t", index);
      if (first == std::string::npos) return false;
      if (content.compare(first, 3, "```") == 0 ||
          content.compare(first, 3, "~~~") == 0) {
        fenced = !fenced;
      }
      line_start = false;
    }
    char current = content[index];
    if (current == '\n' || current == '\r') {
      line_start = true;
      inline_code = false;
      ++index;
      continue;
    }
    if (fenced) {
      ++index;
      continue;
    }
    if (current == '`') {
      inline_code = !inline_code;
      ++index;
      continue;
    }
    if (current == '[' && !inline_code) {
      if (BracketedToolCallMarker(content, index)) return true;
      ++index;
      continue;
    }
    if (inline_code || current != '<') {
      ++index;
      continue;
    }
    size_t close = content.find_first_of(">:\r\n", index + 1);
    if (close == std::string::npos || close - index > 256) {
      ++index;
      continue;
    }
    if (IsForeignToolCallOpener(
            std::string_view(content).substr(index, close - index + 1))) {
      return true;
    }
    index = close + 1;
  }
  return false;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_TOOL_PROTOCOL_H_
