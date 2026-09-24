// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STYLE_H_
#define UAGENT_INCLUDE_CORE_STYLE_H_
// Presentation helpers shared by the terminal renderers: styling that survives
// a line break.

#include <string>
#include <string_view>

#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

// Re-emit the opening sequence after every hard newline so each physical line
// carries its own style. A terminal keeps SGR across a soft wrap, but a copied
// transcript, and any consumer that splits on newlines, does not.
inline std::string StyledBlock(std::string_view text, const char* open) {
  if (!g_color || !open || !*open) return std::string(text) + "\n";
  std::string body(text);
  size_t pos = 0;
  while ((pos = body.find('\n', pos)) != std::string::npos) {
    body.insert(pos + 1, open);
    pos += 1 + std::char_traits<char>::length(open);
  }
  return std::string(open) + body + RST() + "\n";
}

inline bool IsMarkdownFence(std::string_view marker) {
  return marker.size() >= 3 && marker.substr(0, 3) == "```";
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STYLE_H_
