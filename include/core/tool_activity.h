// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TOOL_ACTIVITY_H_
#define UAGENT_INCLUDE_CORE_TOOL_ACTIVITY_H_

#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

// The label a run of same-intent calls folds under; empty for intents that
// always keep their own rows. The web uses the same four.
inline std::string GroupLabel(const std::string& intent) {
  return intent == "explore"    ? "Explored"
         : intent == "research" ? "Researched"
         : intent == "verify"   ? "Verified"
         : intent == "edit"     ? "Edited"
                                : "";
}

// One batch is one assistant response. Only adjacent, successful calls of one
// groupable intent with no approval boundary fold together; a failure keeps
// its own row. Call order, never completion order, determines membership.
// Clients render these facts without classifying tools.
inline void GroupToolActivities(std::vector<json>& activities) {
  for (json& activity : activities) activity.erase("group");
  for (size_t begin = 0; begin < activities.size();) {
    const std::string intent = JsonValue(activities[begin], "category", "");
    auto eligible = [&](size_t i) {
      return !GroupLabel(intent).empty() &&
             JsonValue(activities[i], "category", "") == intent &&
             JsonValue(activities[i], "groupable", false) &&
             JsonValue(activities[i], "status", "") == "success";
    };
    if (!eligible(begin)) {
      ++begin;
      continue;
    }
    size_t end = begin + 1;
    while (end < activities.size() && eligible(end)) ++end;
    if (end - begin > 1) {
      const json group = {
          {"id", activities[begin]["id"]},
          {"label", GroupLabel(intent) + " · " + std::to_string(end - begin) +
                        " calls"}};
      for (size_t i = begin; i < end; ++i) activities[i]["group"] = group;
    }
    begin = end;
  }
}

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_TOOL_ACTIVITY_H_
