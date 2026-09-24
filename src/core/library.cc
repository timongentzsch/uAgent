// Copyright 2026 Timon Gentzsch
#include "include/core/library.h"

#include <string>
#include <system_error>

#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {
std::string LibraryChangePath() { return UagentDir("library") + "/changed"; }
void LibraryChanged() {
  std::string error;
  AtomicWriteFile(LibraryChangePath(), MakeSessionId(), kPrivateFileMode, false,
                  error);
}
bool LibraryName(const std::string& name) {
  return !name.empty() && name.size() <= kLibraryNameChars && name != "." &&
         name != ".." && SafeFileComponent(name) == name &&
         name.find_first_of("/\\") == std::string::npos;
}
bool LibraryPath(const std::filesystem::path& root,
                 const std::filesystem::path& path) {
  auto relative =
      path.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() || relative.is_absolute()) return false;
  auto current = root;
  std::error_code error;
  if (std::filesystem::is_symlink(root, error)) return false;
  for (const auto& part : relative) {
    if (part == "..") return false;
    current /= part;
    if (std::filesystem::is_symlink(current, error)) return false;
  }
  return true;
}
}  // namespace uagent
