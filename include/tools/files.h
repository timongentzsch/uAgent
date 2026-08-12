// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_FILES_H_
#define UAGENT_INCLUDE_TOOLS_FILES_H_
// Bounded file inspection and atomic editing declarations.

#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {

ToolErrorCode FileToolError(const std::error_code& error);
ToolResult ToolReadFile(const std::string& path, int64_t offset, int64_t limit);
ToolResult ToolWriteFile(const std::string& path, const std::string& content);
ToolResult ToolWritePrivateFile(const std::string& path,
                                const std::string& content);
std::string StripLineNumbers(const std::string& text);

struct FileEdit {
  std::string old_text;
  std::string new_text;
  bool replace_all = false;
};

ToolResult ToolEditFile(const std::string& path,
                        const std::vector<FileEdit>& edits);
ToolResult ToolEditFile(const std::string& path, const std::string& old_text,
                        const std::string& new_text, bool replace_all = false);
ToolResult ToolListDir(const std::string& path, int64_t offset = 0,
                       int64_t limit = 0, bool include_small_files = false);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_FILES_H_
