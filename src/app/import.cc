// Copyright 2026 Timon Gentzsch
#include "include/app/import.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/library.h"
#include "include/core/limits.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/tools/memory.h"

namespace uagent {
namespace {
namespace fs = std::filesystem;

// Skills sit one or two folders below the root in real trees
// (~/.claude/skills/synced/<id>/<name>), so the search goes a little deeper
// than a flat directory.
constexpr int kImportSkillDepth = 4;
constexpr size_t kImportReadBytes = size_t{512} * 1024;

struct ToolName {
  std::string_view from;
  std::string_view to;
};
constexpr ToolName kToolNames[] = {{"Bash", "run"},
                                   {"shell", "run"},
                                   {"Read", "read_path"},
                                   {"Write", "write_file"},
                                   {"Edit", "edit_file"},
                                   {"MultiEdit", "edit_file"},
                                   {"apply_patch", "edit_file"},
                                   {"Grep", "grep"},
                                   {"Glob", "grep"},
                                   {"WebFetch", "web_fetch"},
                                   {"WebSearch", "web_search"},
                                   {"Task", "subagent"},
                                   {"Agent", "subagent"}};

std::string_view ToolFor(std::string_view word) {
  for (const ToolName& tool : kToolNames) {
    if (tool.from == word) return tool.to;
  }
  return {};
}

bool WordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// A name is a tool reference when it is backticked or followed by "tool";
// "Read the file" is prose and stays.
std::string RewriteWords(const std::string& line,
                         std::set<std::string>& tools) {
  std::string out;
  for (size_t at = 0; at < line.size();) {
    if (!WordChar(line[at])) {
      out += line[at++];
      continue;
    }
    size_t end = at;
    while (end < line.size() && WordChar(line[end])) ++end;
    const std::string word = line.substr(at, end - at);
    const std::string_view to = ToolFor(word);
    const bool quoted = at > 0 && line[at - 1] == '`' && end < line.size() &&
                        (line[end] == '`' || line[end] == '(');
    const bool named =
        line.compare(end, 5, " tool") == 0 &&
        !(end + 5 < line.size() &&
          std::isalpha(static_cast<unsigned char>(line[end + 5])) != 0);
    if (!to.empty() && (quoted || named)) {
      tools.emplace(to);
      out += to;
    } else {
      out += word;
    }
    at = end;
  }
  return out;
}

// `Bash(git add:*), Read` -> `run, read_path`: argument patterns have no
// counterpart, and several names can map to one tool.
std::string RewriteToolList(const std::string& value,
                            std::set<std::string>& tools) {
  std::vector<std::string> kept;
  std::string token;
  int depth = 0;
  auto flush = [&] {
    if (token.empty()) return;
    const std::string_view to = ToolFor(token.substr(0, token.find('(')));
    if (!to.empty()) tools.emplace(to);
    const std::string entry = to.empty() ? token : std::string(to);
    if (std::find(kept.begin(), kept.end(), entry) == kept.end()) {
      kept.push_back(entry);
    }
    token.clear();
  };
  for (char c : value) {
    if (c == '(') ++depth;
    if (c == ')' && depth > 0) --depth;
    if (depth == 0 &&
        (c == ',' || std::isspace(static_cast<unsigned char>(c)))) {
      flush();
    } else {
      token += c;
    }
  }
  flush();
  std::string joined;
  for (const std::string& entry : kept) {
    joined += (joined.empty() ? "" : ", ") + entry;
  }
  return joined;
}

bool ForbidsCopying(const fs::path& dir) {
  // The wording Anthropic's skill licenses use, and the plain forms of it.
  constexpr std::string_view kPhrases[] = {
      "retain copies of these materials", "reproduce or copy these materials",
      "may not copy", "may not redistribute",
      "redistribution is not permitted"};
  for (const char* name : {"LICENSE", "LICENSE.txt", "LICENSE.md"}) {
    const auto text = ReadFile((dir / name).string(), kImportReadBytes);
    if (!text) continue;
    const std::string lower = AsciiLower(*text);
    for (std::string_view phrase : kPhrases) {
      if (lower.find(phrase) != std::string::npos) return true;
    }
  }
  return false;
}

struct SkillFile {
  fs::path relative;
  fs::path source;
};
struct SkillFolder {
  std::vector<SkillFile> files;
  std::string blocked;
};

// Everything a skill folder holds, within the bounds a skill is read with.
// Links count only when they lead to a file inside the folder.
SkillFolder CollectSkillFolder(const fs::path& dir) {
  SkillFolder folder;
  std::error_code ec, probe;
  const fs::path root = fs::canonical(dir, ec);
  if (ec) {
    folder.blocked = "cannot read the folder: " + ec.message();
    return folder;
  }
  if (ForbidsCopying(root)) {
    folder.blocked = "its license does not allow copying";
    return folder;
  }
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec),
      end;
  for (; it != end && !ec; it.increment(ec)) {
    const fs::path relative = it->path().lexically_relative(root);
    if (relative.filename().string().starts_with(".")) {
      it.disable_recursion_pending();
      continue;
    }
    fs::path source = it->path();
    if (it->is_symlink(probe)) {
      source = fs::canonical(source, probe);
      if (probe || !LibraryPath(root, source) ||
          fs::is_directory(source, probe)) {
        folder.blocked = relative.string() + " is a link that cannot be copied";
        return folder;
      }
    }
    if (fs::is_directory(source, probe)) {
      if (it.depth() >= kMaxSkillFileDepth) {
        folder.blocked = "folders nested deeper than " +
                         std::to_string(kMaxSkillFileDepth) + " levels";
        return folder;
      }
      continue;
    }
    if (!fs::is_regular_file(source, probe)) continue;
    if (folder.files.size() >= kMaxSkillFiles) {
      folder.blocked = "more than " + std::to_string(kMaxSkillFiles) + " files";
      return folder;
    }
    if (fs::file_size(source, probe) > kSkillBodyBytes) {
      folder.blocked = relative.string() + " is over " +
                       std::to_string(kSkillBodyBytes) + " bytes";
      return folder;
    }
    folder.files.push_back({relative, source});
  }
  if (ec) folder.blocked = "cannot read the folder: " + ec.message();
  return folder;
}

void FindSkills(const fs::path& dir, int depth, std::vector<fs::path>& found) {
  std::error_code ec, probe;
  if (fs::is_regular_file(dir / "SKILL.md", probe)) {
    found.push_back(dir);
    return;
  }
  if (depth >= kImportSkillDepth) return;
  std::vector<fs::path> children;
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (!it->path().filename().string().starts_with(".") &&
        it->is_directory(probe)) {
      children.push_back(it->path());
    }
  }
  std::sort(children.begin(), children.end());
  for (const fs::path& child : children) {
    if (found.size() >= kMaxCatalogueEntries) return;
    FindSkills(child, depth + 1, found);
  }
}

json SkillItem(const std::string& source, const fs::path& dir) {
  const fs::path manifest = dir / "SKILL.md";
  const std::string name = SafeFileComponent(dir.filename().string());
  std::string description;
  std::ifstream input(manifest);
  ParseSkillFrontMatter(input, &description);
  std::error_code ec;
  json item = {
      {"key", HashHex(manifest.string())},
      {"kind", "skill"},
      {"source", source},
      {"name", name},
      {"path", manifest.string()},
      {"bytes", fs::file_size(manifest, ec)},
      {"collides", fs::exists(fs::symlink_status(
                       fs::path(GlobalBase()) / "skills" / name, ec))}};
  const std::string blocked = !LibraryName(name) ? "unusable folder name"
                              : Trim(description).empty()
                                  ? "SKILL.md has no description"
                                  : CollectSkillFolder(dir).blocked;
  if (!blocked.empty()) item["blocked"] = blocked;
  return item;
}

// A memory file without its YAML front matter.
std::string MemoryBody(const std::string& text) {
  if (!text.starts_with("---\n")) return Trim(text);
  const size_t close = text.find("\n---", 3);
  const size_t body =
      close == std::string::npos ? close : text.find('\n', close + 1);
  return body == std::string::npos ? Trim(text) : Trim(text.substr(body));
}

json MemoryItem(const std::string& source, const fs::path& path,
                const std::string& blocked,
                const std::vector<MemoryEntry>& existing) {
  std::error_code ec;
  const std::string name = SafeFileComponent(path.stem().string());
  const bool collides = std::any_of(
      existing.begin(), existing.end(),
      [&](const MemoryEntry& e) { return e.key == "project/" + name; });
  json item = {{"key", HashHex(path.string())},
               {"kind", "memory"},
               {"source", source},
               {"name", name},
               {"path", path.string()},
               {"bytes", fs::file_size(path, ec)},
               {"collides", collides}};
  std::string reason = blocked;
  if (source == "claude" && reason.empty()) {
    std::string text, error;
    const auto stored = std::count_if(
        existing.begin(), existing.end(),
        [](const MemoryEntry& e) { return e.key.starts_with("project/"); });
    if (!ReadRegularFile(path.string(), kImportReadBytes, text, error)) {
      reason = error;
    } else if (const size_t size = MemoryBody(text).size();
               size > kMemoryBytes) {
      reason = "its " + std::to_string(size) + " bytes are over the " +
               std::to_string(kMemoryBytes) + " byte limit of a memory";
    } else if (!collides && static_cast<size_t>(stored) >= kMaxMemories) {
      reason = "project memory is full";
    }
  }
  if (!reason.empty()) item["blocked"] = reason;
  return item;
}

json ListItems(const fs::path& cwd) {
  json items = json::array();
  std::set<std::string> seen;
  const std::string home = UserHome();
  std::vector<std::pair<std::string, fs::path>> roots;
  if (!home.empty()) {
    roots.push_back({"claude", fs::path(home) / ".claude" / "skills"});
    roots.push_back({"codex", fs::path(home) / ".codex" / "skills"});
  }
  roots.push_back({"claude", cwd / ".claude" / "skills"});
  roots.push_back({"codex", cwd / ".codex" / "skills"});
  for (const auto& [source, root] : roots) {
    std::vector<fs::path> skills;
    FindSkills(root, 0, skills);
    for (const fs::path& dir : skills) {
      if (seen.insert(dir.string()).second) {
        items.push_back(SkillItem(source, dir));
      }
    }
  }
  if (home.empty()) return items;
  const fs::path own = ClaudeMemoryPath(cwd);
  const std::vector<MemoryEntry> existing =
      ListMemories(cwd, kMaxCatalogueEntries);
  std::error_code ec, probe;
  std::vector<fs::path> projects;
  for (fs::directory_iterator it(fs::path(home) / ".claude" / "projects", ec),
       end;
       it != end && !ec; it.increment(ec)) {
    projects.push_back(it->path() / "memory");
  }
  std::sort(projects.begin(), projects.end());
  for (const fs::path& directory : projects) {
    std::vector<fs::path> files;
    for (fs::directory_iterator it(directory, ec), end; it != end && !ec;
         it.increment(ec)) {
      if (it->is_regular_file(probe) && it->path().extension() == ".md" &&
          it->path().filename() != "MEMORY.md") {
        files.push_back(it->path());
      }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
      if (items.size() >= kMaxCatalogueEntries) return items;
      items.push_back(MemoryItem(
          "claude", file, directory == own ? "" : "belongs to another project",
          existing));
    }
  }
  const fs::path summary =
      fs::path(home) / ".codex" / "memories" / "memory_summary.md";
  if (fs::is_regular_file(summary, ec)) {
    json item = MemoryItem("codex", summary, "", existing);
    item["needs_model"] = true;
    items.push_back(std::move(item));
  }
  return items;
}

std::string ImportSkill(const json& item) {
  const fs::path dir = fs::path(JsonValue(item, "path", "")).parent_path();
  const std::string name = JsonValue(item, "name", "");
  const fs::path destination = fs::path(GlobalBase()) / "skills" / name;
  if (!LibraryPath(GlobalBase(), destination)) return "unsafe skill path";
  std::string error;
  FileLease lease;
  if (!AcquireLibraryWriteLease(lease, error)) return error;
  std::error_code ec;
  if (fs::exists(fs::symlink_status(destination, ec))) {
    return "skill " + name + " already exists";
  }
  SkillFolder folder = CollectSkillFolder(dir);
  if (!folder.blocked.empty()) return folder.blocked;
  // The manifest goes last: the skill appears only once it is complete.
  std::stable_partition(
      folder.files.begin(), folder.files.end(),
      [](const SkillFile& file) { return file.relative != "SKILL.md"; });
  CreatePrivateDirectories(destination);
  for (const SkillFile& file : folder.files) {
    std::string content;
    if (!ReadRegularFile(file.source.string(), kSkillBodyBytes, content,
                         error)) {
      break;
    }
    if (file.relative == "SKILL.md") content = RewriteToolReferences(content);
    const bool executable = (fs::status(file.source, ec).permissions() &
                             fs::perms::owner_exec) != fs::perms::none;
    if (!AtomicWriteFile(
            (destination / file.relative).string(), content,
            static_cast<mode_t>(kPrivateFileMode | (executable ? S_IXUSR : 0)),
            false, error, false)) {
      break;
    }
  }
  if (!error.empty()) {
    fs::remove_all(destination, ec);
    return error;
  }
  LibraryChanged();
  return "";
}

std::string ImportMemory(const json& item, const fs::path& cwd) {
  if (JsonValue(item, "needs_model", false)) {
    return "needs a model to condense it; invoke $import";
  }
  std::string text, error;
  if (!ReadRegularFile(JsonValue(item, "path", ""), kImportReadBytes, text,
                       error)) {
    return error;
  }
  const json saved =
      MemoryControl({{"action", "set"},
                     {"key", "project/" + JsonValue(item, "name", "")},
                     {"revision", ""},
                     {"content", MemoryBody(text)}},
                    cwd);
  return JsonValue(saved, "error", "");
}

}  // namespace

std::string RewriteToolReferences(const std::string& skill_md) {
  std::vector<std::string> lines;
  for (size_t at = 0;;) {
    const size_t end = skill_md.find('\n', at);
    lines.push_back(skill_md.substr(at, end - at));
    if (end == std::string::npos) break;
    at = end + 1;
  }
  size_t close = 0;
  if (Trim(lines[0]) == "---") {
    for (size_t i = 1; i < lines.size() && close == 0; ++i) {
      if (Trim(lines[i]) == "---") close = i;
    }
  }
  std::set<std::string> tools;
  bool has_requires = false, in_list = false;
  for (size_t i = 1; i < close; ++i) {
    const std::string line = Trim(lines[i]);
    has_requires = has_requires || line.starts_with("requires-tools:");
    if (line.starts_with("allowed-tools:")) {
      const std::string list = RewriteToolList(line.substr(14), tools);
      lines[i] = "allowed-tools:" + (list.empty() ? "" : " " + list);
      in_list = true;
    } else if (in_list && line.starts_with("- ")) {
      lines[i] = lines[i].substr(0, lines[i].find('-')) + "- " +
                 RewriteToolList(line.substr(2), tools);
    } else {
      in_list = false;
    }
  }
  for (std::string& line : lines) line = RewriteWords(line, tools);
  if (close > 0 && !has_requires && !tools.empty()) {
    std::string value;
    for (const std::string& tool : tools) {
      value += (value.empty() ? "" : ", ") + tool;
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(close),
                 "requires-tools: " + value);
  }
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
  return out;
}

json ImportControl(const json& request, const fs::path& cwd) {
  const std::string action = JsonValue(request, "action", "list");
  const std::string only = JsonValue(request, "only", "");
  json items = json::array();
  for (json& item : ListItems(cwd)) {
    if (only.empty() || JsonValue(item, "kind", "") == only) {
      items.push_back(std::move(item));
    }
  }
  if (action == "list") return {{"items", items}};
  if (action != "run") return {{"error", "unknown import action"}};
  const std::string key = JsonValue(request, "key", "");
  json imported = json::array(), skipped = json::array();
  for (const json& item : items) {
    if (key != "all" && JsonValue(item, "key", "") != key &&
        JsonValue(item, "name", "") != key) {
      continue;
    }
    const std::string kind = JsonValue(item, "kind", "");
    std::string error = JsonValue(item, "blocked", "");
    if (error.empty() && JsonValue(item, "collides", false)) {
      error = "already exists";
    }
    if (error.empty()) {
      error = kind == "skill" ? ImportSkill(item) : ImportMemory(item, cwd);
    }
    json entry = {{"kind", kind}, {"name", item["name"]}};
    if (error.empty()) {
      imported.push_back(std::move(entry));
    } else {
      entry["reason"] = error;
      skipped.push_back(std::move(entry));
    }
  }
  if (key != "all" && imported.empty()) {
    return {{"error", skipped.empty()
                          ? "nothing to import named " + key
                          : JsonValue(skipped[0], "name", "") + ": " +
                                JsonValue(skipped[0], "reason", "")}};
  }
  return {{"imported", imported}, {"skipped", skipped}};
}
}  // namespace uagent
