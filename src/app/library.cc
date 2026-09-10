// Copyright 2026 Timon Gentzsch
#include "include/app/library.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/core/config.h"
#include "include/core/effective_config.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/skills.h"
#include "include/tools/memory.h"

namespace uagent {
std::string LibraryChangePath() { return UagentDir("library") + "/changed"; }
void LibraryChanged() {
  std::string error;
  AtomicWriteFile(LibraryChangePath(), MakeSessionId(), kPrivateFileMode, false,
                  error);
}
bool LibraryName(const std::string& name) {
  return !name.empty() && name.size() <= 100 && name != "." && name != ".." &&
         SafeFileComponent(name) == name &&
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
json SkillControl(const json& request, const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  const std::string action = JsonValue(request, "action", "list");
  const std::string key = JsonValue(request, "key", "");
  const fs::path global = fs::path(GlobalBase()) / "skills";
  const fs::path project = ProjectBase(cwd) / "skills";
  auto discovered = DiscoverSkills(cwd);
  auto effective = SelectSkills(discovered);
  auto describe = [&](const Skill& skill, bool body) {
    bool own_global = LibraryPath(global, skill.path) &&
                      LibraryPath(GlobalBase(), skill.path);
    bool own_project =
        LibraryPath(project, skill.path) && LibraryPath(cwd, skill.path);
    bool workspace = LibraryPath(cwd, skill.path);
    for (auto parent = cwd.parent_path();
         !workspace && !parent.empty() && parent != parent.parent_path();
         parent = parent.parent_path()) {
      workspace = LibraryPath(parent / ".agents" / "skills", skill.path);
    }
    bool active = std::any_of(
        effective.begin(), effective.end(),
        [&](const Skill& entry) { return entry.path == skill.path; });
    std::string state = skill.description.empty()   ? "invalid"
                        : SkillExcluded(skill.name) ? "disabled"
                        : active                    ? "available"
                                                    : "overridden";
    json item = {
        {"key", HashHex(skill.path)},
        {"name", skill.name},
        {"path", skill.path},
        {"scope", own_project || workspace ? "project" : "global"},
        {"source", own_global || own_project ? "uAgent"
                   : skill.path.find("/share/uagent/") != std::string::npos
                       ? "Bundled"
                       : "External"},
        {"writable", own_global || own_project},
        {"description", skill.description},
        {"status", state},
        {"required_tools", skill.required_tools}};
    std::string content, error;
    if (!ReadRegularFile(skill.path, static_cast<size_t>(SkillBodyBytes()),
                         content, error)) {
      item["error"] = error;
      return item;
    }
    item["revision"] = DocumentRevision(skill.path, content);
    item["bytes"] = content.size();
    item["modified"] = SnapshotFile(skill.path).modified_seconds * 1000;
    if (body) {
      item["content"] = content;
      item["files"] = json::array();
      std::error_code ec;
      fs::recursive_directory_iterator it(
          skill.dir, fs::directory_options::skip_permission_denied, ec),
          end;
      for (; it != end && !ec && item["files"].size() < 128; it.increment(ec)) {
        if (it.depth() >= 4) it.disable_recursion_pending();
        if (fs::is_regular_file(it->symlink_status(ec))) {
          item["files"].push_back(
              it->path().lexically_relative(skill.dir).string());
        }
      }
    }
    return item;
  };
  if (action == "list") {
    json items = json::array();
    for (const auto& skill : discovered) {
      items.push_back(describe(skill, false));
    }
    return {{"items", items},
            {"limit", SkillBodyBytes()},
            {"applies", "new_sessions"}};
  }
  auto found = std::find_if(
      discovered.begin(), discovered.end(),
      [&](const Skill& skill) { return HashHex(skill.path) == key; });
  if (action == "get") {
    return found == discovered.end() ? json{{"error", "skill not found"}}
                                     : json{{"item", describe(*found, true)}};
  }
  if (action == "enable" || action == "disable") {
    if (found == discovered.end()) return {{"error", "skill not found"}};
    std::string error;
    FileLease lease;
    if (!lease.Acquire(UagentDir("library") + "/write.lock", error)) {
      return {{"error", error}};
    }
    auto manager = ConfigManager::Capture(false, {});
    auto settings = manager.Read();
    std::vector<std::string> names =
        SplitPathList(settings.values["UAGENT_SKILL_EXCLUDE"], ',');
    std::erase_if(names, [&](const std::string& name) {
      return Trim(name) == found->name || Trim(name).empty();
    });
    if (action == "disable") names.push_back(found->name);
    std::string value;
    for (const auto& name : names) {
      if (!value.empty()) value += ',';
      value += Trim(name);
    }
    auto result = ConfigurationControl(
        {{"operation", "apply"},
         {"scope", "user"},
         {"changes",
          json::array({{{"key", "UAGENT_SKILL_EXCLUDE"}, {"value", value}}})}},
        manager, settings.config, false);
    if (!result.contains("error")) LibraryChanged();
    return result;
  }
  if (action != "set" && action != "forget") {
    return {{"error", "unknown skill action"}};
  }
  fs::path path;
  if (found != discovered.end()) {
    path = found->path;
    if (!LibraryPath(global, path) && !LibraryPath(project, path)) {
      return {{"error",
               "this skill is managed externally; copy it into a new uAgent "
               "skill to edit"}};
    }
  } else {
    const auto slash = key.find('/');
    std::string scope = key.substr(0, slash);
    if (action != "set" || slash == std::string::npos ||
        (scope != "project" && scope != "global") ||
        !LibraryName(key.substr(slash + 1))) {
      return {{"error", "invalid skill destination"}};
    }
    path = (scope == "project" ? project : global) / key.substr(slash + 1) /
           "SKILL.md";
  }
  const fs::path root = LibraryPath(global, path) ? global : project;
  if (!LibraryPath(root, path) ||
      !LibraryPath(root == global ? fs::path(GlobalBase()) : cwd, path)) {
    return {{"error", "unsafe skill path"}};
  }
  std::string error, previous;
  FileLease lease;
  if (!lease.Acquire(UagentDir("library") + "/write.lock", error)) {
    return {{"error", error}};
  }
  bool existed = PathExists(path.string());
  if (existed &&
      !ReadRegularFile(path.string(), static_cast<size_t>(SkillBodyBytes()),
                       previous, error)) {
    return {{"error", error}};
  }
  if (!request.contains("revision") ||
      JsonValue(request, "revision", "") !=
          (existed ? DocumentRevision(path.string(), previous) : "")) {
    return {{"error", "This skill changed. Reload it before saving."},
            {"conflict", true}};
  }
  if (action == "forget") {
    std::error_code ec;
    // Retain supporting files. Removing the manifest uninstalls the skill;
    // this operation never recursively destroys a directory of user work.
    if (!fs::remove(path, ec)) {
      return {{"error", "cannot remove skill manifest"}};
    }
    fs::remove(path.parent_path(), ec);  // succeeds only if already empty
    LibraryChanged();
    return {{"deleted", key}};
  }
  std::string content = JsonValue(request, "content", ""), description;
  if (content.size() > static_cast<size_t>(SkillBodyBytes())) {
    return {{"error", "skill exceeds configured size limit"}};
  }
  std::istringstream input(content);
  ParseSkillFrontMatter(input, &description, nullptr, nullptr);
  if (Trim(description).empty()) {
    return {{"error", "SKILL.md needs YAML front matter with a description"}};
  }
  CreatePrivateDirectories(path.parent_path());
  if (!AtomicWriteFile(path.string(), content, kPrivateFileMode, true, error)) {
    return {{"error", error}};
  }
  LibraryChanged();
  discovered = DiscoverSkills(cwd);
  effective = SelectSkills(discovered);
  auto saved = std::find_if(
      discovered.begin(), discovered.end(),
      [&](const Skill& skill) { return skill.path == path.string(); });
  return saved == discovered.end()
             ? json{{"error", "saved skill is outside the discovery limit"}}
             : json{{"item", describe(*saved, true)}};
}
json LibraryControl(const json& request,
                    const std::filesystem::path& workspace) {
  std::error_code ec;
  const auto cwd = std::filesystem::canonical(workspace, ec);
  if (ec || !std::filesystem::is_directory(cwd, ec)) {
    return {{"error", "choose an accessible project directory"}};
  }
  if (JsonValue(request, "kind", "") == "memory") {
    return MemoryControl(request, cwd);
  }
  if (JsonValue(request, "kind", "") == "skills") {
    return SkillControl(request, cwd);
  }
  return {{"error", "unknown library kind"}};
}
}  // namespace uagent
