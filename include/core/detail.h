// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_DETAIL_H_
#define UAGENT_INCLUDE_CORE_DETAIL_H_
// The detail levels: how deep a turn's tree starts open in a client.

#include <limits>
#include <string_view>

#include "include/core/config_registry.h"

namespace uagent {

// The setting that names the level.
inline constexpr std::string_view kDetailSetting = "UAGENT_DETAIL";
// A depth that opens every level of a turn's tree.
inline constexpr int kDepthAll = std::numeric_limits<int>::max();

// A level is only a depth: 0 shows each turn's work as one line, 1 opens the
// work to its steps, and all opens everything, routine notices included.
struct DetailLevel {
  std::string_view name;
  int depth;
};

inline constexpr DetailLevel kDetailDepths[] = {{kDetailLevels[0], 0},
                                                {kDetailLevels[1], 1},
                                                {kDetailLevels[2], kDepthAll}};

// An unknown level reads as the default one.
inline const DetailLevel& DetailFor(std::string_view name) {
  for (const DetailLevel& level : kDetailDepths) {
    if (level.name == name) return level;
  }
  return kDetailDepths[1];
}

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_DETAIL_H_
