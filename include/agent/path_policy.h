// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_PATH_POLICY_H_
#define UAGENT_INCLUDE_AGENT_PATH_POLICY_H_
// Which paths a tool may read, write or delete, and how a refused one is
// reported.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/project.h"
#include "include/core/sandbox.h"
#include "include/core/settings_store.h"
#include "include/core/skills.h"
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
  return std::ranges::find(kInstructionNames, name) !=
         std::end(kInstructionNames);
}

// A skill of the user's own is read in every conversation, as their
// instructions are.
inline bool UserSkillPath(const std::filesystem::path& candidate) {
  return std::ranges::any_of(UserSkillPaths(), [&](const auto& skills) {
    return PathWithin(candidate, CanonicalAccessPath(skills.string()));
  });
}

// µAgent's own configuration and trust state: editing it changes what the
// agent may do next launch, so it is never auto-approved. Covers the file
// tools only; the OS sandbox keeps shell commands from the same paths.
inline bool SelfConfigurationPath(const std::string& path) {
  if (path.empty()) return false;
  std::filesystem::path candidate = CanonicalAccessPath(path);
  auto matches = [&](const std::string& target) {
    return !target.empty() && CanonicalAccessPath(target) == candidate;
  };
  // Global instructions steer every session; a folder's are ordinary files.
  if (InstructionFileName(candidate) &&
      candidate.parent_path() == CanonicalAccessPath(GlobalBase())) {
    return true;
  }
  if (UserSkillPath(candidate)) return true;
  if (matches(TrustStorePath()) || matches(SettingsPath()) ||
      matches(SettingsPath() + ".lock") ||
      matches(UagentDir(kConfigDir) + "/" + kPermissionStoreFile)) {
    return true;
  }
  // A folder's .mcp.json decides which servers are spawned: matched by
  // basename in any folder, under either spelling of a symlink.
  return std::filesystem::path(path).filename() == kMcpConfigFile ||
         candidate.filename() == kMcpConfigFile;
}

// The browser profile, uAgent's settings, credential stores: the file tools
// refuse them outright, and the OS sandbox hides them from commands.
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
                       "this path holds credentials or uAgent's own state "
                       "and is not available to tools (sandbox.reveal lifts "
                       "that)",
                       "path");
}

// Checked again when the call runs, not only when it is validated: an earlier
// call in the same batch can have made the path a link to a protected one.
inline Tool::Run RefusingHidden(Tool::Run run) {
  return [run = std::move(run)](const json& args, const ToolContext& context) {
    if (auto refused = RefuseHidden(args)) {
      return ToolFailure(ToolErrorCode::kPermissionDenied, refused->message);
    }
    return run(args, context);
  };
}

// A path this session's commands may not read (SandboxReadable): the file
// tools ask a person for it, so no automatic approval reads it either.
inline bool UnreadablePath(const std::string& path) {
  const SandboxStatus& status = SandboxRuntime();
  return !path.empty() && status.mode == SandboxMode::kEnforced &&
         !SandboxReadable(status.policy, CanonicalAccessPath(path).string());
}

enum class PathAccess { kRead, kWrite };

// Reads escalate too: config files and .mcp.json carry keys and credentials.
// The trust store, instruction files and skills hold no secret, so only
// writing them escalates. A closed folder asks for either.
inline ApprovalClass PathApprovalClass(const std::string& path,
                                       PathAccess access) {
  if (UnreadablePath(path)) return ApprovalClass::kMandatoryHuman;
  if (!SelfConfigurationPath(path)) return ApprovalClass::kNone;
  if (access == PathAccess::kRead &&
      (CanonicalAccessPath(path) == CanonicalAccessPath(TrustStorePath()) ||
       InstructionFileName(CanonicalAccessPath(path)) ||
       UserSkillPath(CanonicalAccessPath(path)))) {
    return ApprovalClass::kNone;
  }
  return ApprovalClass::kMandatoryHuman;
}

// The folder and every root sandboxed commands may write: what a command
// could do there unasked, a file tool may do too.
inline bool WithinWorkArea(const std::string& path,
                           const std::filesystem::path& workspace) {
  const std::filesystem::path canonical = CanonicalAccessPath(path);
  if (PathWithin(canonical, workspace)) return true;
  const SandboxStatus& status = SandboxRuntime();
  if (status.mode != SandboxMode::kEnforced) return false;
  // Roots are recorded as configured (/tmp is /private/tmp on macOS), so
  // both sides are compared resolved.
  const auto within = [&](const std::vector<std::string>& roots) {
    return std::ranges::any_of(roots, [&](const std::string& root) {
      return PathWithin(canonical, CanonicalAccessPath(root));
    });
  };
  return within(status.policy.writable_roots) &&
         !within(status.policy.withheld);
}

inline bool PathApprovalRequired(const std::string& path,
                                 const std::filesystem::path& workspace) {
  return !WithinWorkArea(path, workspace);
}

inline std::optional<ToolResult> ValidatePathTarget(const std::string& path,
                                                    PathTarget target) {
  namespace fs = std::filesystem;
  if (path.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "path must not be empty");
  }

  std::error_code link_error;
  fs::file_status link = fs::symlink_status(path, link_error);
  if (link_error == std::errc::no_such_file_or_directory ||
      link.type() == fs::file_type::not_found) {
    if (target == PathTarget::kWritableFile) return std::nullopt;
    return ToolFailure(ToolErrorCode::kNotFound,
                       "path does not exist: " + path);
  }
  if (link_error) {
    return ToolFailure(ToolErrorCode::kInternal,
                       "cannot inspect " + path + ": " + link_error.message());
  }

  std::error_code status_error;
  fs::file_status status = fs::status(path, status_error);
  if (status_error) {
    if (link.type() == fs::file_type::symlink &&
        status_error == std::errc::no_such_file_or_directory) {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "refusing dangling symlink: " + path);
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
                       "not a directory: " + path);
  }
  if (fs::is_regular_file(status)) return std::nullopt;
  return ToolFailure(ToolErrorCode::kPermissionDenied,
                     "refusing non-regular file: " + path);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_PATH_POLICY_H_
