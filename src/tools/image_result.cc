// Copyright 2026 Timon Gentzsch

#include "include/tools/image_result.h"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "include/core/env.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent {

ToolResult ToolImageResult(const json& content, std::string source_call_id,
                           const char* source, const char* directory) {
  if (!content.contains("data") || !content["data"].is_string()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       std::string("error: ") + source + " image is missing base64 data");
  }
  std::string mime = JsonValue(content, "mimeType", "image/png");
  std::string extension = ImageExtension(mime);
  if (extension.empty()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       std::string("error: unsupported ") + source +
                           " image type " + mime);
  }
  int64_t limit_mb = AttachmentLimitMb();
  std::string bytes;
  if (!Base64Decode(content["data"].get_ref<const std::string&>(), bytes,
                    static_cast<size_t>(limit_mb) * 1024 * 1024)) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       std::string("error: ") + source +
                           " image is invalid or exceeds " +
                           std::to_string(limit_mb) + " MB");
  }
  static std::atomic<uint64_t> sequence{0};
  std::string path =
      UagentDir(directory) + "/image-" + UtcStamp("%Y%m%dT%H%M%SZ") +
      "-" + std::to_string(getpid()) + "-" + std::to_string(sequence++) +
      extension;
  ToolResult saved = ToolWritePrivateFile(path, bytes);
  if (!saved.Ok()) return saved;
  ToolResult attached = Attachments().Add(path, std::move(source_call_id));
  if (!attached.Ok()) {
    std::string reason = std::move(attached.output);
    if (reason.starts_with(kToolErrorPrefix))
      reason.erase(0, kToolErrorPrefix.size());
    return ToolSuccess(std::string("[") + source + " image saved: " + path +
                       "; not attached: " + reason + "]");
  }
  return ToolSuccess(std::string("[") + source + " image saved: " + path +
                     "; attached — readable in your next step]");
}
}  // namespace uagent
