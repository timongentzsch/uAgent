// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TOOLS_IMAGE_RESULT_H_
#define UAGENT_INCLUDE_TOOLS_IMAGE_RESULT_H_
// Saving an image or file a tool produced and queueing it for the next model
// request. Used by tools and MCP results that return images.

#include <string>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {
// Save a bounded base64 tool image and queue it for the next model request.
ToolResult ToolImageResult(const json& content, std::string source_call_id,
                           const char* source, const char* directory);
// Save `bytes` under `directory` as STEM-<stamp>EXTENSION and queue the file
// for the next model request; the result names it as `what`.
ToolResult ToolAttachedFile(const std::string& bytes, const char* stem,
                            const std::string& extension,
                            const std::string& what, std::string source_call_id,
                            const char* directory);
}  // namespace uagent
#endif
