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
#include <iterator>
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
constexpr size_t kMaxImportEntries = 16384;

// What one listing may still visit and return, across every root: each
// directory entry looked at, in a search or inside a skill, is charged here.
struct Budget {
  size_t entries = kMaxImportEntries;
  size_t items = kMaxImportItems;
  size_t bytes = kMaxImportBytes;
  bool truncated = false;

  bool Entry() {
    if (entries > 0) {
      --entries;
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
// counterpart, and several names can map to one tool. The result is the plain
// comma list the loader reads, however the source wrote it (quoted, [a, b]).
std::string RewriteToolList(std::string value, std::set<std::string>& tools) {
  value = Trim(value);
  if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
    value = value.substr(1, value.size() - 2);
  }
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
  return joined;
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

// Why the folder's license forbids copying it, or why it cannot be told. The
// terms may be written in the front matter's `license:` field, or in a file
// that field names, or in the usual files.
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
  auto forbids = [&](const std::string& text) {
    const std::string squashed = Squash(text);
    return std::any_of(std::begin(kPhrases), std::end(kPhrases),
                       [&](std::string_view phrase) {
                         return squashed.find(phrase) != std::string::npos;
                       });
  };
  const std::string field = FrontMatterValue(manifest, "license");
  if (forbids(field)) return "its license does not allow copying";
  std::vector<std::string> names = {"LICENSE", "LICENSE.txt", "LICENSE.md",
                                    "COPYING"};
  std::vector<std::string> named;
  // `license: Proprietary. legal/TERMS.txt has complete terms` names its file.
  std::istringstream words(field);
  for (std::string word; words >> word;) {
    while (!word.empty() && std::strchr(".,;:)\"'", word.back()) != nullptr) {
      word.pop_back();
    }
    const std::string lower = AsciiLower(word);
    const std::string leaf = fs::path(word).filename().string();
    if (lower.ends_with(".txt") || lower.ends_with(".md") ||
        lower.ends_with(".rst") || leaf.starts_with("LICENSE") ||
        leaf.starts_with("COPYING")) {
      named.push_back(word);
    }
  }
  std::error_code ec;
  for (const std::string& name : names) {
    if (!fs::exists(fs::symlink_status(root / name, ec))) continue;
    named.push_back(name);
  }
  for (const std::string& name : named) {
    const fs::path path = root / name;
    const bool inside = fs::path(name).is_relative() &&
                        LibraryPath(root, path.lexically_normal());
    if (!inside || !fs::exists(fs::symlink_status(path, ec))) {
      return "license file not found: " + name;
    }
    std::string text, error;
    if (!ReadRegularFile(path.string(), kImportReadBytes, text, error)) {
      return name + " cannot be read: " + error;
    }
    if (forbids(text)) return "its license does not allow copying";
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
                      Budget& budget, SkillFolder& folder) {
  std::error_code ec, probe;
  fs::directory_iterator it(dir, ec), end;
  for (; !ec && it != end && folder.blocked.empty(); it.increment(ec)) {
    const fs::path& path = it->path();
    if (path.filename() == ".git") continue;
    const std::string relative = path.lexically_relative(root).string();
    if (!budget.Entry()) {
      folder.blocked = "too many entries to inspect";
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
        CollectDirectory(root, path, depth + 1, budget, folder);
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
SkillFolder CollectSkillFolder(const fs::path& dir, const std::string& manifest,
                               Budget& budget) {
  SkillFolder folder;
  std::error_code ec;
  const fs::path root = fs::canonical(dir, ec);
  if (ec) {
    folder.blocked = "cannot read the folder: " + ec.message();
    return folder;
  }
  folder.blocked = LicenseBlock(root, manifest);
  if (folder.blocked.empty()) CollectDirectory(root, root, 0, budget, folder);
  return folder;
}

// A skill folder wanted by `selector` (a key or name; empty or "all" wants
// every one) and still to be imported when `fresh` is set.
struct Wanted {
  std::string selector;
  bool fresh = false;

  bool Name(const std::string& name, const std::string& key) const {
    return selector.empty() || selector == "all" || selector == name ||
           selector == key;
  }
};

void FindSkills(const fs::path& dir, int depth, Budget& budget,
                const Wanted& wanted, std::vector<fs::path>& found) {
  std::error_code ec, probe;
  auto take = [&](const fs::path& skill) {
    const std::string name = SafeFileComponent(skill.filename().string());
    std::error_code gone;
    if (!wanted.Name(name, HashHex((skill / "SKILL.md").string())) ||
        (wanted.fresh &&
         fs::exists(fs::symlink_status(fs::path(GlobalBase()) / "skills" / name,
                                       gone)))) {
      return;
    }
    found.push_back(skill);
  };
  if (fs::is_regular_file(dir / "SKILL.md", probe)) {
    take(dir);
    return;
  }
  if (depth >= kImportSkillDepth) return;
  std::vector<fs::path> children;
  fs::directory_iterator it(dir, ec), end;
  for (; it != end && !ec && budget.Entry(); it.increment(ec)) {
    if (it->path().filename().string().starts_with(".")) continue;
    // A folder reached through a link is a candidate itself, never searched.
    const fs::file_status status = it->symlink_status(probe);
    if (fs::is_directory(status)) {
      children.push_back(it->path());
    } else if (fs::is_symlink(status) &&
               fs::is_regular_file(it->path() / "SKILL.md", probe)) {
      take(it->path());
    }
  }
  std::sort(children.begin(), children.end());
  for (const fs::path& child : children) {
    if (budget.items <= found.size()) {
      budget.truncated = true;
      return;
    }
    FindSkills(child, depth + 1, budget, wanted, found);
  }
}

json SkillItem(const std::string& source, const fs::path& root,
               const fs::path& dir, Budget& budget) {
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
    blocked = CollectSkillFolder(dir, text, budget).blocked;
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
                const std::string& blocked, bool collides, size_t stored) {
  std::error_code ec;
  const std::string name = SafeFileComponent(path.stem().string());
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
    if (!ReadRegularFile(path.string(), kImportReadBytes, text, error)) {
      reason = error;
    } else if (const size_t size = MemoryBody(text).size();
               size > kMemoryBytes) {
      reason = "its " + std::to_string(size) + " bytes are over the " +
               std::to_string(kMemoryBytes) + " byte limit of a memory";
    } else if (!collides && stored >= kMaxMemories) {
      reason = "project memory is full";
    }
  }
  if (!reason.empty()) item["blocked"] = reason;
  return item;
}

// What can be imported, within one traversal budget. `only` limits which
// sources are walked at all, and `wanted` which candidates become items
// before they count against the cap.
json ListItems(const fs::path& cwd, const std::string& only,
               const Wanted& wanted, bool& truncated) {
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
      FindSkills(root, 0, budget, wanted, skills);
      for (const fs::path& dir : skills) {
        if (seen.insert(dir.string()).second &&
            !budget.Add(items, SkillItem(source, root, dir, budget))) {
          break;
        }
      }
    }
  }
  if (only != "skill" && !home.empty()) {
    const fs::path own = ClaudeMemoryPath(cwd);
    const std::vector<MemoryEntry> existing =
        ListMemories(cwd, kMaxCatalogueEntries);
    const auto stored = static_cast<size_t>(std::count_if(
        existing.begin(), existing.end(),
        [](const MemoryEntry& e) { return e.key.starts_with("project/"); }));
    auto add = [&](const std::string& source, const fs::path& file,
                   const std::string& blocked) {
      const std::string name = SafeFileComponent(file.stem().string());
      const bool collides = std::any_of(
          existing.begin(), existing.end(),
          [&](const MemoryEntry& e) { return e.key == "project/" + name; });
      if (!wanted.Name(name, HashHex(file.string())) ||
          (wanted.fresh && collides)) {
        return true;
      }
      json item = MemoryItem(source, file, blocked, collides, stored);
      if (source == "codex") item["needs_model"] = true;
      return budget.Add(items, std::move(item));
    };
    std::error_code ec, probe;
    std::vector<fs::path> projects;
    for (fs::directory_iterator it(fs::path(home) / ".claude" / "projects", ec),
         end;
         it != end && !ec && budget.Entry(); it.increment(ec)) {
      projects.push_back(it->path() / "memory");
    }
    std::sort(projects.begin(), projects.end());
    for (const fs::path& directory : projects) {
      std::vector<fs::path> files;
      for (fs::directory_iterator it(directory, ec), end;
           it != end && !ec && budget.Entry(); it.increment(ec)) {
        if (it->is_regular_file(probe) && it->path().extension() == ".md" &&
            it->path().filename() != "MEMORY.md") {
          files.push_back(it->path());
        }
      }
      std::sort(files.begin(), files.end());
      for (const fs::path& file : files) {
        if (!add("claude", file,
                 directory == own ? "" : "belongs to another project")) {
          break;
        }
      }
    }
    const fs::path summary =
        fs::path(home) / ".codex" / "memories" / "memory_summary.md";
    if (fs::is_regular_file(summary, ec)) add("codex", summary, "");
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
  Budget budget;
  const SkillFolder folder = CollectSkillFolder(dir, manifest, budget);
  if (!folder.blocked.empty()) return folder.blocked;
  // Copied beside the skills and moved into place whole, so the skill appears
  // complete or not at all. The staging folder is outside every searched
  // root, on the same filesystem, and only the one made here is ever removed.
  CreatePrivateDirectories(skills);
  std::string pattern = UagentDir("staging") + "/" + name + ".XXXXXX";
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
  bool has_requires = false;
  std::vector<bool> dropped(lines.size(), false);
  for (size_t i = 1; i < close; ++i) {
    const std::string line = Trim(lines[i]);
    std::string key;
    for (const char* name : {"allowed-tools", "tools", "requires-tools"}) {
      if (line.starts_with(std::string(name) + ":")) key = name;
    }
    if (key.empty()) continue;
    has_requires = has_requires || key == "requires-tools";
    // A block list becomes the one-line list the loader reads.
    std::string value = line.substr(key.size() + 1);
    const bool block = Trim(value).empty();
    for (size_t next = i + 1;
         block && next < close && Trim(lines[next]).starts_with("- "); ++next) {
      value += Trim(lines[next]).substr(2) + ",";
      dropped[next] = true;
    }
    const std::string list = RewriteToolList(value, tools);
    lines[i] = key + ":" + (list.empty() ? "" : " " + list);
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
  for (size_t i = 0; i < lines.size(); ++i) {
    if (dropped[i]) continue;
    if (i > 0) out += '\n';
    out += lines[i];
  }
  return out;
}

json ImportControl(const json& request, const fs::path& cwd) {
  const std::string action = JsonValue(request, "action", "list");
  const std::string only = JsonValue(request, "only", "");
  const std::string key = JsonValue(request, "key", "");
  if (action != "list" && action != "run") {
    return {{"error", "unknown import action"}};
  }
  if (action == "run" && key.empty()) {
    return {{"error", "name what to import, or say all"}};
  }
  // A named item is looked for directly, so the listing's cap never hides it;
  // "all" leaves out what is already imported, so a repeat reaches the rest.
  bool truncated = false;
  const json items = ListItems(
      cwd, only, {action == "list" ? "" : key, key == "all"}, truncated);
  if (action == "list") return {{"items", items}, {"truncated", truncated}};
  json imported = json::array(), skipped = json::array();
  for (const json& item : items) {
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
  return {
      {"imported", imported}, {"skipped", skipped}, {"truncated", truncated}};
}
}  // namespace uagent
