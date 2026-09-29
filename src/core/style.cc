// Copyright 2026 Timon Gentzsch

#include "include/core/style.h"

#include <string>
#include <string_view>

#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

std::string Note(Tone tone, std::string_view text) {
  const char* style = tone == Tone::kError  ? RED()
                      : tone == Tone::kWarn ? YEL()
                                            : DIM();
  return StyledBlock(AsciiGlyphs("· " + std::string(text)), style);
}

}  // namespace uagent
