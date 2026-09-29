// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PATH_POLICY_H_
#define UAGENT_INCLUDE_AGENT_PATH_POLICY_H_

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/project.h"
#include "include/core/sandbox.h"
#include "include/tools/tool.h"

namespace uagent {

enum class PathTarget {
  kReadableFile,
  kWritableFile,
  kDeletableFile,
  kDirectory,
};

// Any name an instruction file can have, AGENTS.md's fallbacks included.
inline bool InstructionFileName(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return name == "COORDINATOR.md" ||
         std::ranges::find(kInstructionNames, name) !=
             std::end(kInstructionNames);
}

// µAgent's own configuration and trust state. Editing these changes what the
// agent is allowed to do next launch, so they are never auto-approved. This
// covers the built-in file tools; the OS sandbox is what stops an approved
// shell command from reaching the same paths behind them.
inline bool SelfConfigurationPath(const std::string& path) {
  if (path.empty()) return false;
  std::filesystem::path candidate = CanonicalAccessPath(path);
  auto matches = [&](const std::string& target) {
    return !target.empty() && CanonicalAccessPath(target) == candidate;
  };
  // Instructions outside the repository steer every session (yours) or a
  // coordinator; a project's AGENTS.md is an ordinary repository file.
  if (InstructionFileName(candidate) &&
      (candidate.filename() == "COORDINATOR.md"
           ? candidate.parent_path().filename() == ".uagent"
           : candidate.parent_path() == CanonicalAccessPath(GlobalBase()))) {
    return true;
  }
  if (matches(UagentConfigPath()) || matches(ProjectConfigFilePath()) ||
      matches(TrustStorePath()) || matches(EnvStr("UAGENT_CONFIG_FILE")) ||
      matches(UagentDir(kConfigDir) + "/" + kPermissionStoreFile)) {
    return true;
  }
  // A workspace .mcp.json decides which servers are spawned. Preserve the
  // basename rule for nested workspaces while recognizing either spelling of
  // a symlink.
  return std::filesystem::path(path).filename() == ".mcp.json" ||
         candidate.filename() == ".mcp.json";
}

// The browser profile (HiddenPaths): the file tools refuse it outright rather
// than asking, and the OS sandbox hides it from commands.
inline bool HiddenPath(const std::string& path) {
  if (path.empty()) return false;
  const std::filesystem::path candidate = CanonicalAccessPath(path);
  for (const std::string& hidden : HiddenPaths()) {
    if (PathWithin(candidate, hidden)) return true;
  }
  return false;
}

inline std::optional<ToolArgumentIssue> RefuseHidden(const json& args) {
  if (!HiddenPath(JsonValue(args, "path", "."))) return std::nullopt;
  return ArgumentIssue("policy.hidden",
                       "the browser profile is private to the browser", "path");
}

// Checked again when the call runs, not only when it is validated: an earlier
// call in the same batch can have made the path a link to a protected one.
inline Tool::Run RefusingHidden(Tool::Run run) {
  return [run = std::move(run)](const json& args, const ToolContext& context) {
    if (auto refused = RefuseHidden(args)) {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "error: " + refused->message);
    }
    return run(args, context);
  };
}

enum class PathAccess { kRead, kWrite };

// Reads escalate as well as writes: the user and project config files and a
// workspace .mcp.json carry provider keys and server credentials, so pulling
// one into context is itself the harm. The trust store and instruction files
// are the exceptions -- they hold no secret, so only writing them changes
// what the agent may do next launch.
inline ApprovalClass PathApprovalClass(const std::string& path,
                                       PathAccess access) {
  if (!SelfConfigurationPath(path)) return ApprovalClass::kNone;
  if (access == PathAccess::kRead &&
      (CanonicalAccessPath(path) == CanonicalAccessPath(TrustStorePath()) ||
       InstructionFileName(CanonicalAccessPath(path)))) {
    return ApprovalClass::kNone;
  }
  return ApprovalClass::kMandatoryHuman;
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

#endif  // UAGENT_INCLUDE_AGENT_PATH_POLICY_H_
