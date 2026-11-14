// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_DETAIL_H_
#define UAGENT_INCLUDE_CORE_DETAIL_H_
// The detail levels: how deep a turn's tree starts open in a client.

#include <limits>
#include <string_view>

#include "include/core/config_registry.h"

namespace uagent {

inline constexpr std::string_view kDetailSetting = "UAGENT_DETAIL";
// Whether the model's thinking is drawn at all, at any level.
inline constexpr std::string_view kThinkingSetting = "UAGENT_THINKING";
inline constexpr int kDepthAll = std::numeric_limits<int>::max();

// A level is only a depth: 0 shows each turn's work as one line, 1 opens the
// work to its steps, and all opens everything, routine rows included. A new
// install starts at the first.
struct DetailLevel {
  std::string_view name;
  int depth;
};

inline constexpr DetailLevel kDetailDepths[] = {{kDetailLevels[0], 0},
                                                {kDetailLevels[1], 1},
                                                {kDetailLevels[2], kDepthAll}};

// An unknown level reads as the one a new install starts at.
inline const DetailLevel& DetailFor(std::string_view name) {
  for (const DetailLevel& level : kDetailDepths) {
    if (level.name == name) return level;
  }
  return kDetailDepths[0];
}
// The level after `level`, the first after the last.
inline const DetailLevel& NextDetail(const DetailLevel& level) {
  const DetailLevel* next = &level + 1;
  return next == std::end(kDetailDepths) ? kDetailDepths[0] : *next;
}

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_DETAIL_H_
