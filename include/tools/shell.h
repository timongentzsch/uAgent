// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SHELL_H_
#define UAGENT_INCLUDE_TOOLS_SHELL_H_
// Supervised shell, isolated uv Python, and repository search declarations.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/core/child_env.h"
#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

struct ShellCommandResult {
  ToolResult result;
  std::optional<int> wait_status = std::nullopt;
  bool launched = false;
};

struct ShellCommand {
  std::string command;
  // Run directly, through no shell; `command` then only names the activity.
  std::vector<std::string> argv = {};
  std::string shell = "bash";
  bool background = false;
  bool detach = false;
  bool immediate = false;
  bool tty = false;
  // Confined by the OS sandbox when the session enforces one. On by default,
  // so a new call site is confined unless it opts out.
  bool sandbox = true;
  int64_t yield_ms = 0;
  int64_t max_output_chars = 0;
  ActivityKind activity_kind = ActivityKind::kCommand;
  std::string activity_label = {};
  std::string receipt_path = {};
  std::string source_id = {};
  EnvironmentOverrides environment = {};
  ChildEnvironmentPolicy environment_policy =
      ChildEnvironmentPolicy::kSanitized;
  // From RootCommand: the activity answers sudo's prompt `nonce` with `secret`.
  std::string nonce = {};
  std::shared_ptr<std::string> secret = nullptr;
};

// `command` under sudo, over ssh when `host` is named; `sudo -n` with no nonce.
std::string RootCommand(const std::string& command, const std::string& host,
                        bool tty, const std::string& nonce);

// Whether sudo, on `host` over ssh when named, asks this user for no password.
bool PasswordlessSudo(const std::string& host);

ShellCommandResult RunShellCommand(ActivitySupervisor& supervisor,
                                   const ToolContext& context,
                                   ShellCommand command);

struct GrepOptions {
  std::string glob{};
  int64_t context_lines = 0;
  bool files_only = false;      // match the pattern against file paths
  bool literal = false;         // fixed text, not a regex
  bool matching_files = false;  // list the files with a match, not the lines
};
ToolResult ToolGrep(ActivitySupervisor& supervisor, const std::string& pattern,
                    const std::string& path, const GrepOptions& options = {},
                    const ToolContext& context = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SHELL_H_
