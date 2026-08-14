// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SHELL_H_
#define UAGENT_INCLUDE_TOOLS_SHELL_H_
// Supervised shell, isolated uv Python, and repository search declarations.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "include/core/child_env.h"
#include "include/core/json.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

struct ShellCommandResult {
  ToolResult result;
  std::optional<int> wait_status = std::nullopt;
};

struct ShellCommand {
  std::string command;
  std::string shell = "bash";
  bool background = false;
  bool detach = false;
  bool immediate = false;
  bool tty = false;
  int64_t yield_ms = 0;
  int64_t max_output_chars = 0;
  std::string job_kind = {};
  std::string activity_label = {};
  std::string receipt_path = {};
  std::string source_id = {};
  EnvironmentOverrides environment = {};
  ChildEnvironmentPolicy environment_policy =
      ChildEnvironmentPolicy::kSanitized;
};

ShellCommandResult RunShellCommand(ProcessSupervisor& supervisor,
                                   const ToolContext& context,
                                   ShellCommand command);

ToolResult ToolRunApprovedShell(ProcessSupervisor& supervisor,
                                const std::string& command,
                                const ToolContext& context, bool detach,
                                const std::string& shell, bool tty = false,
                                int64_t yield_ms = 0,
                                int64_t max_output_chars = 0);
bool StartsWithShellWord(const std::string& command, const std::string& word);
std::string RunCommandPolicyError(const std::string& command);
bool PythonScriptHasDependencies(const std::string& source);
ToolResult ToolRunPython(ProcessSupervisor& supervisor,
                         const std::filesystem::path& workspace,
                         const std::string& relative_path, const json& code,
                         const json& packages, const ToolContext& context = {});
ToolResult ToolGrep(ProcessSupervisor& supervisor, const std::string& pattern,
                    const std::string& path, const std::string& glob,
                    int64_t context_lines = 0, const ToolContext& context = {},
                    bool files_only = false);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SHELL_H_
