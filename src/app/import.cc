// Copyright 2026 Timon Gentzsch
#include "include/app/import.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
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
// A listing is one JSON line the control process must pass on within its 1 MiB
// output limit; the walk stops at either bound, however many roots it spans.
constexpr size_t kMaxImportItems = 512;
constexpr size_t kMaxImportBytes = size_t{512} * 1024;

// What one listing may still visit and return, across every root.
struct Budget {
  size_t directories = kMaxCatalogueEntries;
  size_t items = kMaxImportItems;
  size_t bytes = kMaxImportBytes;
  bool truncated = false;

  bool Directory() {
    if (directories > 0) {
      --directories;
      return true;
    }
    truncated = true;
    return false;
  }
  bool Add(json& list, json item) {
    const size_t size = item.dump().size();
    if (items == 0 || size > bytes) {
      truncated = true;
      return false;
    }
    --items;
    bytes -= size;
    list.push_back(std::move(item));
    return true;
  }
};

struct ToolName {
  std::string_view from;
  std::string_view to;
  // A word that is also ordinary prose or a class name: a tool reference only
  // where the text says "tool" or the front matter lists tools.
  bool ambiguous;
};
constexpr ToolName kToolNames[] = {{"Bash", "run", false},
                                   {"exec_command", "run", false},
                                   {"shell", "run", true},
                                   {"Read", "read_path", true},
                                   {"Write", "write_file", true},
                                   {"Edit", "edit_file", true},
                                   {"MultiEdit", "edit_file", false},
                                   {"apply_patch", "edit_file", false},
                                   {"Grep", "grep", false},
                                   {"Glob", "grep", false},
                                   {"WebFetch", "web_fetch", false},
                                   {"WebSearch", "web_search", false},
                                   {"Task", "subagent", true},
                                   {"Agent", "subagent", true}};

const ToolName* ToolFor(std::string_view word) {
  for (const ToolName& tool : kToolNames) {
    if (tool.from == word) return &tool;
  }
  return nullptr;
}

bool WordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// A backticked name is a tool reference when nothing else it could be uses
// that name; any name is one when followed by "tool". "Read the file" is prose
// and stays, and so does the Python class `Task`.
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
    const ToolName* tool = ToolFor(word);
    const bool quoted = at > 0 && line[at - 1] == '`' && end < line.size() &&
                        (line[end] == '`' || line[end] == '(');
    const bool named =
        line.compare(end, 5, " tool") == 0 &&
        !(end + 5 < line.size() &&
          std::isalpha(static_cast<unsigned char>(line[end + 5])) != 0);
    if (tool && (named || (quoted && !tool->ambiguous))) {
      tools.emplace(tool->to);
      out += tool->to;
    } else {
      out += word;
    }
    at = end;
  }
  return out;
}

// `Bash(ls:*), Read` -> `run, read_path`: argument patterns have no
// counterpart, and several names can map to one tool. Quoted strings and
// [a, b] arrays keep their wrapper.
std::string RewriteToolList(std::string value, std::set<std::string>& tools) {
  value = Trim(value);
  std::string open, close;
  if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
    open = "[";
    close = "]";
  } else if (value.size() >= 2 &&
             (value.front() == '"' || value.front() == '\'') &&
             value.back() == value.front()) {
    open = close = std::string(1, value.front());
  }
  if (!open.empty()) value = value.substr(1, value.size() - 2);
  std::vector<std::string> kept;
  std::string token;
  int depth = 0;
  auto flush = [&] {
    while (!token.empty() && (token.back() == '"' || token.back() == '\'')) {
      token.pop_back();
    }
    token.erase(0, token.find_first_not_of("\"'"));
    if (token.empty()) return;
    const ToolName* tool = ToolFor(token.substr(0, token.find('(')));
    if (tool) tools.emplace(tool->to);
    const std::string entry = tool ? std::string(tool->to) : token;
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
  return joined.empty() && open.empty() ? "" : open + joined + close;
}

std::string FrontMatterValue(const std::string& text, std::string_view key) {
  std::istringstream input(text);
  std::string line;
  if (!std::getline(input, line) || Trim(line) != "---") return "";
  while (std::getline(input, line) && Trim(line) != "---") {
    const size_t colon = line.find(':');
    if (colon != std::string::npos && Trim(line.substr(0, colon)) == key) {
      return Unquote(Trim(line.substr(colon + 1)));
    }
  }
  return "";
}

// Lower case, with every run of white space one space, so wording that wraps
// across lines still matches.
std::string Squash(const std::string& text) {
  std::string out;
  bool space = false;
  for (char c : AsciiLower(text)) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

// Why the folder's license forbids copying it, or why it cannot be told.
std::string LicenseBlock(const fs::path& root, const std::string& manifest) {
  // The wording Anthropic's skill licenses use, and the plain forms of it.
  constexpr std::string_view kPhrases[] = {"retain copies of these materials",
                                           "reproduce or copy these materials",
                                           "may not copy",
                                           "may not reproduce",
                                           "may not redistribute",
                                           "may not be copied",
                                           "copying is not permitted",
                                           "redistribution is not permitted"};
  std::vector<std::string> names = {"LICENSE", "LICENSE.txt", "LICENSE.md",
                                    "COPYING"};
  // `license: Proprietary. LICENSE.txt has complete terms` names its file.
  std::istringstream words(FrontMatterValue(manifest, "license"));
  for (std::string word; words >> word;) {
    while (!word.empty() && std::strchr(".,;:)\"'", word.back()) != nullptr) {
      word.pop_back();
    }
    if (word.find('.') != std::string::npos &&
        word.find_first_of("/\\") == std::string::npos &&
        !word.starts_with(".")) {
      names.push_back(word);
    }
  }
  std::error_code ec;
  for (const std::string& name : names) {
    if (!fs::exists(fs::symlink_status(root / name, ec))) continue;
    std::string text, error;
    if (!ReadRegularFile((root / name).string(), kImportReadBytes, text,
                         error)) {
      return name + " cannot be read: " + error;
    }
    const std::string squashed = Squash(text);
    for (std::string_view phrase : kPhrases) {
      if (squashed.find(phrase) != std::string::npos) {
        return "its license does not allow copying";
      }
    }
  }
  return "";
}

struct SkillFile {
  fs::path relative;
  fs::path source;
};
struct SkillFolder {
  std::vector<SkillFile> files;
  std::string blocked;
};

void CollectDirectory(const fs::path& root, const fs::path& dir, int depth,
                      SkillFolder& folder, size_t& entries) {
  std::error_code ec, probe;
  fs::directory_iterator it(dir, ec), end;
  for (; !ec && it != end && folder.blocked.empty(); it.increment(ec)) {
    const fs::path& path = it->path();
    if (path.filename() == ".git") continue;
    const std::string relative = path.lexically_relative(root).string();
    if (++entries > kMaxCatalogueEntries) {
      folder.blocked =
          "more than " + std::to_string(kMaxCatalogueEntries) + " entries";
      return;
    }
    fs::file_status status = it->symlink_status(probe);
    fs::path source = path;
    if (fs::is_symlink(status)) {
      // A link counts only when it leads to a file inside the folder.
      source = fs::canonical(path, probe);
      status = fs::status(source, probe);
      if (probe || !LibraryPath(root, source) || fs::is_directory(status)) {
        folder.blocked = relative + " is a link that cannot be copied";
        return;
      }
    }
    if (fs::is_directory(status)) {
      if (depth >= kMaxSkillFileDepth) {
        folder.blocked = "folders nested deeper than " +
                         std::to_string(kMaxSkillFileDepth) + " levels";
      } else if (!LibraryPath(root, path)) {
        folder.blocked = relative + " leaves the folder";
      } else {
        CollectDirectory(root, path, depth + 1, folder, entries);
      }
    } else if (!fs::is_regular_file(status)) {
      folder.blocked = relative + " is not a regular file";
    } else if (folder.files.size() >= kMaxSkillFiles) {
      folder.blocked = "more than " + std::to_string(kMaxSkillFiles) + " files";
    } else if (fs::file_size(source, probe) > kSkillBodyBytes) {
      folder.blocked =
          relative + " is over " + std::to_string(kSkillBodyBytes) + " bytes";
    } else {
      folder.files.push_back({path.lexically_relative(root), source});
    }
  }
  if (ec && folder.blocked.empty()) {
    folder.blocked = "cannot read " + dir.lexically_relative(root).string() +
                     ": " + ec.message();
  }
}

// Everything a skill folder holds, within the bounds a skill is read with:
// hidden files too, but never `.git`, and nothing that cannot be copied is
// passed over in silence.
SkillFolder CollectSkillFolder(const fs::path& dir,
                               const std::string& manifest) {
  SkillFolder folder;
  std::error_code ec;
  const fs::path root = fs::canonical(dir, ec);
  if (ec) {
    folder.blocked = "cannot read the folder: " + ec.message();
    return folder;
  }
  folder.blocked = LicenseBlock(root, manifest);
  size_t entries = 0;
  if (folder.blocked.empty()) CollectDirectory(root, root, 0, folder, entries);
  return folder;
}

void FindSkills(const fs::path& dir, int depth, size_t limit, Budget& budget,
                std::vector<fs::path>& found) {
  std::error_code ec, probe;
  if (fs::is_regular_file(dir / "SKILL.md", probe)) {
    found.push_back(dir);
    return;
  }
  if (depth >= kImportSkillDepth || !budget.Directory()) return;
  std::vector<fs::path> children;
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (it->path().filename().string().starts_with(".")) continue;
    // A folder reached through a link is a candidate itself, never searched.
    const fs::file_status status = it->symlink_status(probe);
    if (fs::is_directory(status)) {
      children.push_back(it->path());
    } else if (fs::is_symlink(status) &&
               fs::is_regular_file(it->path() / "SKILL.md", probe)) {
      found.push_back(it->path());
    }
  }
  std::sort(children.begin(), children.end());
  for (const fs::path& child : children) {
    if (found.size() >= limit) {
      budget.truncated = true;
      return;
    }
    FindSkills(child, depth + 1, limit, budget, found);
  }
}

json SkillItem(const std::string& source, const fs::path& root,
               const fs::path& dir) {
  const fs::path manifest = dir / "SKILL.md";
  const std::string name = SafeFileComponent(dir.filename().string());
  std::string text, error, description;
  const bool read =
      ReadRegularFile(manifest.string(), kSkillBodyBytes, text, error);
  std::istringstream input(text);
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
  std::string blocked;
  if (!LibraryName(name)) {
    blocked = "unusable folder name";
  } else if (!read) {
    blocked = "SKILL.md: " + error;
  } else if (Trim(description).empty()) {
    blocked = "SKILL.md has no description";
  } else if (fs::is_symlink(dir, ec) &&
             !LibraryPath(fs::canonical(root, ec), fs::canonical(dir, ec))) {
    blocked = "the folder is a link out of " + root.string();
  } else {
    blocked = CollectSkillFolder(dir, text).blocked;
  }
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

// `only` limits which sources are walked at all.
json ListItems(const fs::path& cwd, const std::string& only, bool& truncated) {
  json items = json::array();
  Budget budget;
  std::set<std::string> seen;
  const std::string home = UserHome();
  if (only != "memory") {
    std::vector<std::pair<std::string, fs::path>> roots;
    if (!home.empty()) {
      roots.push_back({"claude", fs::path(home) / ".claude" / "skills"});
      roots.push_back({"codex", fs::path(home) / ".codex" / "skills"});
    }
    roots.push_back({"claude", cwd / ".claude" / "skills"});
    roots.push_back({"codex", cwd / ".codex" / "skills"});
    for (const auto& [source, root] : roots) {
      std::vector<fs::path> skills;
      FindSkills(root, 0, budget.items, budget, skills);
      for (const fs::path& dir : skills) {
        if (seen.insert(dir.string()).second &&
            !budget.Add(items, SkillItem(source, root, dir))) {
          break;
        }
      }
    }
  }
  if (only != "skill" && !home.empty()) {
    const fs::path own = ClaudeMemoryPath(cwd);
    const std::vector<MemoryEntry> existing =
        ListMemories(cwd, kMaxCatalogueEntries);
    std::error_code ec, probe;
    std::vector<fs::path> projects;
    for (fs::directory_iterator it(fs::path(home) / ".claude" / "projects", ec),
         end;
         it != end && !ec && budget.Directory(); it.increment(ec)) {
      projects.push_back(it->path() / "memory");
    }
    std::sort(projects.begin(), projects.end());
    for (const fs::path& directory : projects) {
      std::vector<fs::path> files;
      for (fs::directory_iterator it(directory, ec), end;
           it != end && !ec && budget.Directory(); it.increment(ec)) {
        if (it->is_regular_file(probe) && it->path().extension() == ".md" &&
            it->path().filename() != "MEMORY.md") {
          files.push_back(it->path());
        }
      }
      std::sort(files.begin(), files.end());
      for (const fs::path& file : files) {
        const std::string blocked =
            directory == own ? "" : "belongs to another project";
        if (!budget.Add(items, MemoryItem("claude", file, blocked, existing))) {
          break;
        }
      }
    }
    const fs::path summary =
        fs::path(home) / ".codex" / "memories" / "memory_summary.md";
    if (fs::is_regular_file(summary, ec)) {
      json item = MemoryItem("codex", summary, "", existing);
      item["needs_model"] = true;
      budget.Add(items, std::move(item));
    }
  }
  truncated = budget.truncated;
  return items;
}

std::string ImportSkill(const json& item) {
  const fs::path dir = fs::path(JsonValue(item, "path", "")).parent_path();
  const std::string name = JsonValue(item, "name", "");
  const fs::path skills = fs::path(GlobalBase()) / "skills";
  const fs::path destination = skills / name;
  if (!LibraryPath(GlobalBase(), destination)) return "unsafe skill path";
  std::string error, manifest;
  FileLease lease;
  if (!AcquireLibraryWriteLease(lease, error)) return error;
  std::error_code ec;
  if (fs::exists(fs::symlink_status(destination, ec))) {
    return "skill " + name + " already exists";
  }
  if (!ReadRegularFile((dir / "SKILL.md").string(), kSkillBodyBytes, manifest,
                       error)) {
    return "SKILL.md: " + error;
  }
  const SkillFolder folder = CollectSkillFolder(dir, manifest);
  if (!folder.blocked.empty()) return folder.blocked;
  // Copied beside the destination and moved into place whole, so the skill
  // appears complete or not at all. Only the folder made here is ever removed.
  CreatePrivateDirectories(skills);
  std::string pattern = (skills / ("." + name + ".import-XXXXXX")).string();
  if (mkdtemp(pattern.data()) == nullptr) {
    return std::string("cannot create a staging folder: ") + strerror(errno);
  }
  const fs::path staging = pattern;
  for (const SkillFile& file : folder.files) {
    std::string content, failure;
    if (!ReadRegularFile(file.source.string(), kSkillBodyBytes, content,
                         failure)) {
      error = file.relative.string() + ": " + failure;
      break;
    }
    if (file.relative == "SKILL.md") content = RewriteToolReferences(content);
    const bool executable = (fs::status(file.source, ec).permissions() &
                             fs::perms::owner_exec) != fs::perms::none;
    if (!AtomicWriteFile(
            (staging / file.relative).string(), content,
            static_cast<mode_t>(kPrivateFileMode | (executable ? S_IXUSR : 0)),
            false, error, false)) {
      break;
    }
  }
  if (error.empty() && fs::exists(fs::symlink_status(destination, ec))) {
    error = "skill " + name + " already exists";
  }
  if (error.empty()) {
    fs::rename(staging, destination, ec);
    if (ec) error = "cannot move the skill into place: " + ec.message();
  }
  if (!error.empty()) {
    fs::remove_all(staging, ec);
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
    std::string key;
    for (const char* name : {"allowed-tools", "tools", "requires-tools"}) {
      if (line.starts_with(std::string(name) + ":")) key = name;
    }
    if (!key.empty()) {
      has_requires = has_requires || key == "requires-tools";
      const std::string list =
          RewriteToolList(line.substr(key.size() + 1), tools);
      lines[i] = key + ":" + (list.empty() ? "" : " " + list);
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
  bool truncated = false;
  const json items = ListItems(cwd, only, truncated);
  if (action == "list") return {{"items", items}, {"truncated", truncated}};
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
