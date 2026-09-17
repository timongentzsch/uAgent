// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_FILE_SERVICES_H_
#define UAGENT_INCLUDE_AGENT_FILE_SERVICES_H_
// Policy-checked atomic file writes shared by session persistence,
// collaborator records, asset stores and the file tools. The path policy
// refuses symlinks and non-regular files; the write itself is crash-safe via
// AtomicWriteFile. Tool handlers and domain writers use these rather than
// reaching past each other's layers. Bodies live in
// src/agent/file_services.cc.

#include <sys/types.h>

#include <string>
#include <system_error>

#include "include/tools/tool.h"

namespace uagent {

ToolErrorCode FileToolError(const std::error_code& error);
ToolResult ToolAtomicWrite(const std::string& path, const std::string& content,
                           mode_t create_mode, bool preserve_mode,
                           bool overwrite = true);
ToolResult ToolWriteFileMode(const std::string& path,
                             const std::string& content, mode_t create_mode);
ToolResult ToolWritePrivateFile(const std::string& path,
                                const std::string& content);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_FILE_SERVICES_H_
