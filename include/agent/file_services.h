// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_FILE_SERVICES_H_
#define UAGENT_INCLUDE_AGENT_FILE_SERVICES_H_
// Policy-checked atomic file writes: the path policy refuses symlinks and
// non-regular files.

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
