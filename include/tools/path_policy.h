// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_PATH_POLICY_H_
#define UAGENT_INCLUDE_TOOLS_PATH_POLICY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/tools/tool.h"

namespace uagent {

enum class PathTarget {
  kReadableFile,
  kWritableFile,
  kDirectory,
};

// µAgent's own configuration and trust state. Editing these changes what the
// agent is allowed to do next launch, so they are never auto-approved. This is
// defense in depth for the built-in file tools, not a sandbox: an approved
// shell command can still reach the same paths.
inline bool SelfConfigurationPath(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::path candidate = std::filesystem::absolute(path, ec);
  if (ec) return false;
  candidate = candidate.lexically_normal();
  auto matches = [&](const std::string& target) {
    if (target.empty()) return false;
    std::error_code target_ec;
    std::filesystem::path resolved =
        std::filesystem::absolute(target, target_ec).lexically_normal();
    return !target_ec && resolved == candidate;
  };
  if (matches(UagentConfigPath()) || matches(ProjectConfigFilePath()) ||
      matches(TrustStorePath()) || matches(EnvStr("UAGENT_CONFIG_FILE"))) {
    return true;
  }
  // A workspace .mcp.json decides which servers are spawned.
  return candidate.filename() == ".mcp.json";
}

inline ApprovalClass PathApprovalClass(const std::string& path) {
  return SelfConfigurationPath(path) ? ApprovalClass::kMandatoryHuman
                                     : ApprovalClass::kNone;
}

inline bool PathApprovalRequired(const std::string& path,
                                 const std::filesystem::path& workspace) {
  return !PathWithin(CanonicalAccessPath(path), workspace);
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
    if (link.type() == fs::file_type::symlink &&
        status_error == std::errc::no_such_file_or_directory) {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "error: refusing dangling symlink: " + path);
    }
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
