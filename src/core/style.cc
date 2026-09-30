// Copyright 2026 Timon Gentzsch

#include "include/core/style.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

std::string JoinDot(std::initializer_list<std::string_view> parts) {
  std::string joined;
  for (std::string_view part : parts) {
    if (part.empty()) continue;
    if (!joined.empty()) joined += " · ";
    joined += part;
  }
  return joined;
}

std::string Note(Tone tone, std::string_view text) {
  const char* style = tone == Tone::kError  ? RED()
                      : tone == Tone::kWarn ? YEL()
                                            : DIM();
  const Mark mark = tone == Tone::kError  ? Mark::kError
                    : tone == Tone::kWarn ? Mark::kWarn
                                          : Mark::kNote;
  return StyledBlock(RowMark(mark) + AsciiGlyphs(text), style);
}

std::string RowMark(Mark mark) {
  static constexpr std::pair<std::string_view, std::string_view> kMarks[] = {
      {"→ ", "tool: "},   {"← ", "result: "},  {"← ", "failed: "},
      {"• ", "status: "}, {"• ", "changed: "}, {"◆ skill ", "skill: "},
      {"· ", ""},         {"· ", "warning: "}, {"· ", "error: "},
  };
  const auto& [glyph, label] = kMarks[static_cast<size_t>(mark)];
  return std::string(g_plain ? label : AsciiGlyphs(glyph));
}

}  // namespace uagent
