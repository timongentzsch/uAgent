// Copyright 2026 Timon Gentzsch

#include "include/agent/file_services.h"

#include <sys/types.h>

#include <string>
#include <system_error>
#include <utility>

#include "include/agent/path_policy.h"
#include "include/core/fs.h"
#include "include/tools/tool.h"

namespace uagent {

ToolErrorCode FileToolError(const std::error_code& error) {
  if (error == std::errc::no_such_file_or_directory) {
    return ToolErrorCode::kNotFound;
  }
  if (error == std::errc::permission_denied ||
      error == std::errc::operation_not_permitted ||
      error == std::errc::read_only_file_system) {
    return ToolErrorCode::kPermissionDenied;
  }
  return ToolErrorCode::kInternal;
}

ToolResult ToolAtomicWrite(const std::string& path, const std::string& content,
                           mode_t create_mode, bool preserve_mode,
                           bool overwrite) {
  std::string error;
  if (!AtomicWriteFile(path, content, create_mode, preserve_mode, error,
                       overwrite)) {
    return ToolFailure(ToolErrorCode::kInternal, "error: " + error);
  }
  return ToolSuccess("wrote " + std::to_string(content.size()) + " bytes to " +
                     path);
}

// Atomic write: temp file in the same directory, then rename — a disk-full or
// crash mid-write can never leave the target truncated. Keeps an existing
// file's permissions.
ToolResult ToolWriteFileMode(const std::string& path,
                             const std::string& content, mode_t create_mode) {
  if (auto invalid = ValidatePathTarget(path, PathTarget::kWritableFile)) {
    return std::move(*invalid);
  }
  return ToolAtomicWrite(path, content, create_mode, /*preserve_mode=*/true);
}

ToolResult ToolWritePrivateFile(const std::string& path,
                                const std::string& content) {
  return ToolWriteFileMode(path, content, kPrivateFileMode);
}

}  // namespace uagent
