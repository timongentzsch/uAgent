// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_FILES_H_
#define UAGENT_INCLUDE_TOOLS_FILES_H_
// Bounded file inspection and atomic editing declarations.

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {

ToolErrorCode FileToolError(const std::error_code& error);
ToolResult ToolAtomicWrite(const std::string& path, const std::string& content,
                           mode_t create_mode, bool preserve_mode);
ToolResult ToolReadFile(const std::string& path, int64_t offset, int64_t limit);
ToolResult ToolWriteFile(const std::string& path, const std::string& content);
ToolResult ToolWriteFileWithDisplay(const std::string& path,
                                    const std::string& content);
ToolResult ToolDeleteFileWithDisplay(const std::string& path);
std::optional<std::string> DiffableContents(const std::string& path);
// +/- receipt for a whole-file write; empty when the content is unchanged.
std::string WholeFileDiffDisplay(const std::string& path,
                                 const std::string& previous,
                                 const std::string& content, bool existed);
std::string DeletedFileDiffDisplay(const std::string& path,
                                   const std::string& previous);
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
// Apply `edits` to `data` in memory exactly as ToolEditFile would, returning
// the refusal it would report. A caller that has to predict an edit — the
// approval prompt — shows what will happen instead of its own guess at it.
std::optional<ToolResult> ApplyFileEdits(std::string& data,
                                         const std::string& path,
                                         const std::vector<FileEdit>& edits);
ToolResult ToolEditFile(const std::string& path, const std::string& old_text,
                        const std::string& new_text, bool replace_all = false);
ToolResult ToolListDir(const std::string& path, int64_t offset = 1,
                       int64_t limit = 0, bool include_small_files = false);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_FILES_H_
