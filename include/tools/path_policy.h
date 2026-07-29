// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_PATH_POLICY_H_
#define UAGENT_INCLUDE_TOOLS_PATH_POLICY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "include/core/fs.h"
#include "include/tools/tool.h"

namespace uagent {

enum class PathTarget {
  kReadableFile,
  kWritableFile,
  kDirectory,
};

inline bool PathApprovalRequired(const std::string& path,
                                 const std::filesystem::path& workspace,
                                 const char* fallback = "") {
  return !PathWithin(
      CanonicalAccessPath(path.empty() ? std::string(fallback) : path),
      workspace);
}

inline std::optional<ToolResult> ValidatePathTarget(const std::string& path,
                                                    PathTarget target) {
  namespace fs = std::filesystem;
  if (path.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: path must not be empty");
  }

  std::error_code link_error;
  fs::file_status link = fs::symlink_status(path, link_error);
  if (link_error == std::errc::no_such_file_or_directory ||
      link.type() == fs::file_type::not_found) {
    if (target == PathTarget::kWritableFile) return std::nullopt;
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: path does not exist: " + path);
  }
  if (link_error) {
    return ToolFailure(
        ToolErrorCode::kInternal,
        "error: cannot inspect " + path + ": " + link_error.message());
  }

  std::error_code status_error;
  fs::file_status status = fs::status(path, status_error);
  if (status_error) {
    return ToolFailure(
        status_error == std::errc::permission_denied
            ? ToolErrorCode::kPermissionDenied
            : ToolErrorCode::kInternal,
        "error: cannot inspect " + path + ": " + status_error.message());
  }
  if (target == PathTarget::kDirectory) {
    if (fs::is_directory(status)) return std::nullopt;
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: not a directory: " + path);
  }
  if (fs::is_regular_file(status)) return std::nullopt;
  return ToolFailure(ToolErrorCode::kPermissionDenied,
                     "error: refusing non-regular file: " + path);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PATH_POLICY_H_
