// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SELF_INFO_H_
#define UAGENT_INCLUDE_TOOLS_SELF_INFO_H_

#include <functional>
#include <string>

#include "include/app/self_description.h"
#include "include/tools/tool.h"

namespace uagent {

// Supplied by the application so the tool always answers from live state
// rather than a snapshot taken at registration time.
using SelfDescriptionProvider =
    std::function<json(SelfTopic, const std::string&)>;

Tool SelfInfoTool(SelfDescriptionProvider describe);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SELF_INFO_H_
