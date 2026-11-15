// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_TOOL_PROTOCOL_H_
#define UAGENT_INCLUDE_API_TOOL_PROTOCOL_H_
// Detects tool-call markup the harness does not execute, so the turn loop
// can suppress it. Nothing here turns markup into a call.

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

// A `[...]` marker counts only when arguments follow it immediately: prose
// says "[tool call] failed" and means it.
inline bool BracketedToolCallMarker(std::string_view text, size_t open) {
  size_t close = text.find(']', open + 1);
  if (close == std::string_view::npos || close - open > 256) return false;
  if (close + 1 >= text.size() || text[close + 1] != '{') return false;
  return IsForeignToolCallOpener(text.substr(open, close - open + 1));
}

// Only the leading non-whitespace bytes: foreign call syntax starts at the
// response boundary. `complete` resolves a final partial marker.
inline LeadingToolMarkup ClassifyLeadingToolMarkup(std::string_view content,
                                                   bool complete = false) {
  size_t start = content.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return complete ? LeadingToolMarkup::kProse : LeadingToolMarkup::kUndecided;
  }
  std::string_view visible = content.substr(start);
  if (visible.front() == '[') {
    // Undecided until the byte after the closing bracket arrives.
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

// The same for a whole message; Markdown code examples are ignored.
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

#endif  // UAGENT_INCLUDE_API_TOOL_PROTOCOL_H_
