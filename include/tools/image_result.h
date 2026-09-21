// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TOOLS_IMAGE_RESULT_H_
#define UAGENT_INCLUDE_TOOLS_IMAGE_RESULT_H_

#include <string>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {
// Save a bounded base64 tool image and queue it for the next model request.
ToolResult ToolImageResult(const json& content, std::string source_call_id,
                           const char* source, const char* directory);
}  // namespace uagent
#endif
