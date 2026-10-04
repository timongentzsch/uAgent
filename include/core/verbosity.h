// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_VERBOSITY_H_
#define UAGENT_INCLUDE_CORE_VERBOSITY_H_

#include <string_view>

#include "include/core/config_registry.h"
#include "include/core/json.h"

namespace uagent {

// The setting that names the level.
inline constexpr std::string_view kVerbositySetting = "UAGENT_VERBOSITY";

// How a turn's tool work is laid out: one row for the whole turn, one per
// group of like calls, or one per call.
enum class WorkRows { kTurn, kGroups, kCalls };
// Thinking text: absent, there to open (a terminal has no such row and
// leaves it out), or shown.
enum class Thinking { kHidden, kClosed, kOpen };

// What one UAGENT_VERBOSITY level shows. A presentation choice, never a
// runtime one: the terminal and the web read this table and hold no rule of
// their own, so retuning a level is one row here.
struct DetailPolicy {
  std::string_view level;
  WorkRows work;
  Thinking reasoning;
  // A call's arguments and output are shown without asking.
  bool open;
  // Routine notices and memory receipts are shown.
  bool minor;
};

// In the order of kVerbosityLevels.
inline constexpr DetailPolicy kDetailPolicies[] = {
    {"minimal", WorkRows::kTurn, Thinking::kHidden, false, false},
    {"default", WorkRows::kGroups, Thinking::kClosed, false, false},
    {"full", WorkRows::kCalls, Thinking::kOpen, true, true},
};

// An unknown level reads as the default one.
inline const DetailPolicy& DetailFor(std::string_view level) {
  for (const DetailPolicy& policy : kDetailPolicies) {
    if (policy.level == level) return policy;
  }
  return kDetailPolicies[1];
}

// The table as the web receives it, keyed by level.
inline json DetailPoliciesJson() {
  json levels = json::object();
  for (const DetailPolicy& policy : kDetailPolicies) {
    levels[std::string(policy.level)] = {
        {"work", policy.work == WorkRows::kTurn     ? "turn"
                 : policy.work == WorkRows::kGroups ? "groups"
                                                    : "calls"},
        {"reasoning", policy.reasoning == Thinking::kHidden   ? "hidden"
                      : policy.reasoning == Thinking::kClosed ? "closed"
                                                              : "open"},
        {"open", policy.open},
        {"minor", policy.minor}};
  }
  return levels;
}

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_VERBOSITY_H_
