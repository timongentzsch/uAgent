// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TOOL_ACTIVITY_H_
#define UAGENT_INCLUDE_CORE_TOOL_ACTIVITY_H_

#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

// One batch is one assistant response. Only adjacent, successful exploration
// with no approval boundary folds together. Call order, never completion order,
// determines membership. Clients render these facts without classifying tools.
inline void GroupToolActivities(std::vector<json>& activities) {
  for (json& activity : activities) activity.erase("group");
  for (size_t begin = 0; begin < activities.size();) {
    auto eligible = [&](size_t i) {
      return JsonValue(activities[i], "category", "") == "explore" &&
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
          {"label", "Explored · " + std::to_string(end - begin) + " calls"}};
      for (size_t i = begin; i < end; ++i) activities[i]["group"] = group;
    }
    begin = end;
  }
}

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_TOOL_ACTIVITY_H_
