// Copyright 2026 Timon Gentzsch

#include "include/mcp/result.h"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/time.h"
#include "include/media.h"
#include "include/tools/files.h"
#include "include/tools/tool.h"

namespace uagent {

ToolResult McpImageResult(const json& content) {
  if (!content.contains("data") || !content["data"].is_string()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: MCP image is missing base64 data");
  }
  std::string mime = JsonValue(content, "mimeType", "image/png");
  std::string extension = ImageExtension(mime);
  if (extension.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: unsupported MCP image type " + mime);
  }
  int64_t limit_mb = TerminalImageLimitMb();
  std::string bytes;
  if (!Base64Decode(content["data"].get_ref<const std::string&>(), bytes,
                    static_cast<size_t>(limit_mb) * 1024 * 1024)) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: MCP image is invalid or exceeds " +
                           std::to_string(limit_mb) + " MB");
  }
  static std::atomic<uint64_t> sequence{0};
  std::string path =
      UagentDir(kMcpDir) + "/image-" + UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
      std::to_string(getpid()) + "-" + std::to_string(sequence++) + extension;
  ToolResult saved = ToolWritePrivateFile(path, bytes);
  if (!saved.Ok()) return saved;
  // A tool result is text-only, so the image cannot travel back inside it.
  // Queue it instead: it rides in on the next request and the model can
  // actually look at it. Printing it to the terminal was never what made it
  // readable — that only showed it to the human, on every single call.
  ToolResult attached = Attachments().Add(path);
  if (!attached.Ok()) {
    std::string reason = std::move(attached.output);
    constexpr std::string_view kErrorPrefix = "error: ";
    if (reason.starts_with(kErrorPrefix)) reason.erase(0, kErrorPrefix.size());
    return ToolSuccess("[mcp image saved: " + path +
                       "; not attached: " + reason + "]");
  }
  return ToolSuccess(
      "[mcp image saved: " + path +
      "; attached — readable in your next step. Use show_image to put it on "
      "the user's terminal]");
}

}  // namespace uagent
