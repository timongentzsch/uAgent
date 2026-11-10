// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_WORKSPACE_FILES_H_
#define UAGENT_INCLUDE_APP_WORKSPACE_FILES_H_
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/core/json.h"
namespace uagent::workspace_files {
bool Shown(const std::filesystem::path& path);
json Folders(const std::string& path);
std::vector<std::string> Candidates(const std::filesystem::path& root,
                                    bool& more);
std::optional<std::filesystem::path> Resolve(const std::filesystem::path& root,
                                             const std::string& relative);
std::optional<json> List(const std::filesystem::path& root,
                         const std::filesystem::path& folder);
json Find(const std::filesystem::path& root, const std::string& query);
struct Change {
  std::string path;
  char state;
  bool untracked;
};
struct Changes {
  bool repository = false;
  std::vector<Change> list;
  bool more = false;
};
Changes ReadChanges(const std::filesystem::path& folder,
                    const std::string& pathspec = ".");
std::optional<json> ReadDiff(const std::filesystem::path& folder,
                             const Changes& changes, const std::string& path);
std::string PreviewMime(const std::filesystem::path& path);
std::string DeckPage(const std::filesystem::path& folder, int width,
                     int height);
// Only filesystem operations. The main retains HTTP authentication, preview
// capabilities, CSP and rendering; both Local and remote use these handlers.
json Request(const std::filesystem::path& root, const json& request);
}  // namespace uagent::workspace_files
#endif
