// Copyright 2026 Timon Gentzsch

#include "include/core/style.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

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

std::string KeyValueRow(std::string_view label, std::string_view value,
                        size_t indent) {
  constexpr size_t kLabelColumns = 16;
  std::string row(indent, ' ');
  row += DIM();
  row += label;
  row.append(kLabelColumns - std::min(label.size(), kLabelColumns), ' ');
  row += RST();
  row += ' ';
  row += value;
  return row + '\n';
}

std::string Note(Tone tone, std::string_view text) {
  const char* style = tone == Tone::kError  ? RED()
                      : tone == Tone::kWarn ? YEL()
                                            : DIM();
  return StyledBlock(AsciiGlyphs("· " + std::string(text)), style);
}

}  // namespace uagent
