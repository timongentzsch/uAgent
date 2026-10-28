// Copyright 2026 Timon Gentzsch

#include "include/tools/image_result.h"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"

namespace uagent {

ToolResult ToolImageResult(const json& content, std::string source_call_id,
                           const char* source, const char* directory) {
  if (!content.contains("data") || !content["data"].is_string()) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       std::string(source) + " image is missing base64 data");
  }
  std::string mime = JsonValue(content, "mimeType", kPngMime);
  std::string extension = ImageExtension(mime);
  if (extension.empty()) {
    return ToolFailure(
        ToolErrorCode::kRemoteError,
        std::string("unsupported ") + source + " image type " + mime);
  }
  int64_t limit_mb = AttachmentLimitMb();
  std::string bytes;
  if (!Base64Decode(content["data"].get_ref<const std::string&>(), bytes,
                    MiB(static_cast<size_t>(limit_mb)))) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       std::string(source) + " image is invalid or exceeds " +
                           std::to_string(limit_mb) + " MB");
  }
  return ToolAttachedFile(bytes, "image", extension,
                          std::string(source) + " image",
                          std::move(source_call_id), directory);
}

ToolResult ToolAttachedFile(const std::string& bytes, const char* stem,
                            const std::string& extension,
                            const std::string& what, std::string source_call_id,
                            const char* directory) {
  static std::atomic<uint64_t> sequence{0};
  std::string path = UagentDir(directory) + "/" + stem + "-" +
                     UtcStamp("%Y%m%dT%H%M%SZ") + "-" +
                     std::to_string(getpid()) + "-" +
                     std::to_string(sequence++) + extension;
  ToolResult saved = ToolWritePrivateFile(path, bytes);
  if (!saved.Ok()) return saved;
  ToolResult attached = Attachments().Add(path, std::move(source_call_id));
  // The row shows the file itself; where it went is for the model.
  ToolResult result = ToolSuccess("");
  result.body_kind = "image";
  if (!attached.Ok()) {
    std::string reason = std::move(attached.output);
    if (reason.starts_with(kToolErrorPrefix)) {
      reason.erase(0, kToolErrorPrefix.size());
    }
    result.hint =
        "[" + what + " saved: " + path + "; not attached: " + reason + "]";
  } else {
    result.hint = "[" + what + " saved: " + path +
                  "; attached — readable in your next step]";
  }
  return result;
}
}  // namespace uagent
