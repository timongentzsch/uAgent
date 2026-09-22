// Copyright 2026 Timon Gentzsch

#include "include/mcp/result.h"

#include <string>
#include <utility>

#include "include/core/fs.h"
#include "include/tools/image_result.h"

namespace uagent {
ToolResult McpImageResult(const json& content, std::string source_call_id) {
  return ToolImageResult(content, std::move(source_call_id), "mcp", kMcpDir);
}
}  // namespace uagent
