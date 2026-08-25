// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STYLE_H_
#define UAGENT_INCLUDE_CORE_STYLE_H_
// Presentation helpers shared by the terminal renderers: styling that survives
// a line break, and the ticker that chases a live edge.

#include <algorithm>
#include <chrono>
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

// Reasoning arrives far faster than any readable scroll rate, so a fixed rate
// falls behind without bound. The cursor chases the live edge instead, moving
// at least kRollColsPerSec and closing the gap within kCatchUpSeconds:
// staleness is bounded in time, not in columns.
//
// When the stream outruns the window no motion is readable, so the ticker
// follows the edge rather than sliding. That choice is made from the edge's
// speed, not the accumulated gap: deciding per gap drifts and then snaps by
// whole windows, which reads worse than either mode.
struct EdgeChaser {
  double cursor = 0;
  double edge = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  static constexpr double kRollColsPerSec = 14.0;
  static constexpr double kCatchUpSeconds = 0.5;
  static constexpr double kReadableFraction = 0.5;

  double advance(double target, size_t cols,
                 std::chrono::steady_clock::time_point now) {
    double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    double arrived = target - edge;
    edge = target;
    double gap = target - cursor;
    if (arrived > static_cast<double>(cols) * kReadableFraction || gap <= 0) {
      // Past the live edge: the first frame, a trimmed buffer, or a stream
      // moving too fast for motion to read as motion.
      cursor = target;
    } else {
      cursor = std::min(
          target,
          cursor + std::max(kRollColsPerSec, gap / kCatchUpSeconds) * seconds);
    }
    return cursor;
  }
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STYLE_H_
