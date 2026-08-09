// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SKILLS_H_
#define UAGENT_INCLUDE_CORE_SKILLS_H_
// Skill discovery. A skill is a directory holding SKILL.md: front matter
// naming it, then the procedure itself. Only the front matter is read at
// startup. Discovery is deferred behind one fixed tool schema, and the body
// arrives only when the model selects a skill.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {

struct Skill {
  std::string name;         // directory name, and what the model calls
  std::string description;  // front-matter summary used during discovery
  std::string dir;          // absolute, so SKILL.md can reference siblings
  std::string path;         // the SKILL.md itself
  std::vector<std::string> required_tools;
  std::string argument_hint;
};

struct SkillReadResult {
  bool ok = false;
  std::string output;
};

// `key: value` pairs between the opening and closing `---`. Enough YAML for
// scalar metadata and a comma-separated tool dependency list; other keys are
// ignored.
inline void ParseSkillFrontMatter(
    std::istream& input, std::string& name, std::string& description,
    std::vector<std::string>* required_tools = nullptr,
    std::string* argument_hint = nullptr) {
  std::string line;
  if (!std::getline(input, line) || Trim(line) != "---") return;
  while (std::getline(input, line)) {
    if (Trim(line) == "---") return;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = Trim(line.substr(0, colon));
    std::string value = Unquote(Trim(line.substr(colon + 1)));
    if (key == "name") {
      name = std::move(value);
    } else if (key == "description") {
      description = std::move(value);
    } else if (key == "requires-tools" && required_tools) {
      for (std::string tool : SplitPathList(value, ',')) {
        tool = Trim(tool);
        if (!tool.empty()) required_tools->push_back(std::move(tool));
      }
    } else if (key == "argument-hint" && argument_hint) {
      *argument_hint = std::move(value);
    }
  }
}

// SKILL.md is an open format that ~30 agents read from their own directory, so
// a skill installed for any of them is already on the machine and usable here.
// User-level paths first, then the workspace's, and ours last in each group:
// later wins, so a project overrides a user skill and µAgent's own overrides a
// vendor copy of the same name. UAGENT_SKILL_PATH replaces the whole list.
inline std::vector<std::filesystem::path> SkillSearchPath(
    const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  std::vector<fs::path> path;
  std::string custom = EnvStr("UAGENT_SKILL_PATH");
  if (!custom.empty()) {
    for (const std::string& entry : SplitPathList(custom)) {
      if (!Trim(entry).empty()) path.push_back(fs::path(Trim(entry)));
    }
    return path;
  }
  // ".agents" is the vendor-neutral location; the others are where Claude Code
  // and Codex keep theirs.
  constexpr const char* kVendors[] = {".agents", ".claude", ".codex"};
  std::string home = UserHome();
  if (!home.empty()) {
    for (const char* vendor : kVendors) {
      path.push_back(fs::path(home) / vendor / "skills");
    }
  }
  path.push_back(fs::path(GlobalBase()) / "skills");
  // Vendor-neutral project skills apply from every ancestor. Walk from the
  // filesystem root toward cwd so the nearest repository scope wins.
  std::vector<fs::path> ancestors;
  for (fs::path current = fs::absolute(cwd); !current.empty();) {
    ancestors.push_back(current);
    fs::path parent = current.parent_path();
    if (parent == current) break;
    current = std::move(parent);
  }
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    path.push_back(*it / ".agents" / "skills");
  }
  // Tool-specific locations remain workspace-local compatibility roots.
  for (const char* vendor : {".claude", ".codex"}) {
    path.push_back(cwd / vendor / "skills");
  }
  path.push_back(ProjectBase(cwd) / "skills");
  return path;
}

inline bool SkillExcluded(const std::string& name) {
  for (const std::string& entry :
       SplitPathList(EnvStr("UAGENT_SKILL_EXCLUDE"), ',')) {
    if (Trim(entry) == name) return true;
  }
  return false;
}

inline std::vector<Skill> LoadSkills(const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  std::vector<Skill> found;
  auto scan = [&](const fs::path& base) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec),
        end;
    for (; it != end && !ec; it.increment(ec)) {
      if (it.depth() >= 6) it.disable_recursion_pending();
      if (!it->is_regular_file(ec) || it->path().filename() != "SKILL.md") {
        continue;
      }
      dirs.push_back(it->path().parent_path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& dir : dirs) {
      fs::path file = dir / "SKILL.md";
      std::ifstream input(file);
      if (!input) continue;
      std::string name, description;
      std::vector<std::string> required_tools;
      std::string argument_hint;
      ParseSkillFrontMatter(input, name, description, &required_tools,
                            &argument_hint);
      // The directory name wins: it is what the model names, and it cannot
      // collide with another skill or carry a path separator.
      name = SafeFileComponent(dir.filename().string());
      if (SkillExcluded(name) || Trim(description).empty()) continue;
      description = Utf8Trunc(std::move(description),
                              static_cast<size_t>(SkillDescriptionBytes()));
      argument_hint = Utf8Trunc(OneLine(std::move(argument_hint)), 128);
      Skill skill{name,
                  description,
                  dir.string(),
                  file.string(),
                  std::move(required_tools),
                  std::move(argument_hint)};
      auto same = std::find_if(found.begin(), found.end(), [&](const Skill& s) {
        return s.name == skill.name;
      });
      if (same != found.end()) {
        found.erase(same);
      } else if (static_cast<int64_t>(found.size()) >= MaxSkills()) {
        found.erase(found.begin());
      }
      // Discovery runs from low to high precedence. Moving every accepted
      // skill to the end means a full catalogue evicts the oldest, lowest-
      // precedence entry rather than hiding a workspace skill.
      found.push_back(std::move(skill));
    }
  };
  std::vector<fs::path> seen;
  for (const fs::path& base : SkillSearchPath(cwd)) {
    // The workspace can be the home directory, and a vendor path can repeat;
    // scanning one twice would only cost time, but it would also let a skill
    // shadow itself and read as a precedence bug.
    if (std::find(seen.begin(), seen.end(), base) != seen.end()) continue;
    seen.push_back(base);
    scan(base);
  }
  return found;
}

// The complete body without its front matter, bounded. Oversized skills fail
// explicitly instead of silently giving the model a partial procedure.
inline SkillReadResult ReadSkillBody(const Skill& skill,
                                     std::string arguments = {}) {
  std::ifstream input(skill.path, std::ios::binary);
  if (!input) return {false, "error: cannot read " + skill.path};
  std::string name, description;
  ParseSkillFrontMatter(input, name, description);
  size_t cap = static_cast<size_t>(SkillBodyBytes());
  std::string body(cap + 1, '\0');
  input.read(body.data(), static_cast<std::streamsize>(body.size()));
  size_t read = static_cast<size_t>(input.gcount());
  if (read > cap) {
    return {false, "error: " + skill.path + " exceeds " + std::to_string(cap) +
                       " bytes; raise UAGENT_SKILL_BYTES to load it fully"};
  }
  body.resize(read);
  body = Utf8Prefix(std::move(body), cap);
  if (Trim(body).empty()) {
    return {false, "error: " + skill.path + " has no body"};
  }
  ReplaceAll(body, "${SKILL_DIR}", skill.dir);
  ReplaceAll(body, "$ARGUMENTS", arguments);
  std::string out = "[skill " + skill.name + " — files in " + skill.dir + "]\n";
  if (!arguments.empty()) out += "[arguments: " + arguments + "]\n";
  std::string normalized = std::filesystem::path(skill.path).generic_string();
  if (normalized.find("/.codex/skills/") != std::string::npos ||
      normalized.find("/.claude/skills/") != std::string::npos) {
    out +=
        "[cross-agent compatibility: use only tools currently registered in "
        "µAgent. Treat named vendor tools, MCP servers, packages, and runtime "
        "features as unavailable until verified; adapt the procedure to the "
        "closest registered tool.]\n";
  }
  out += Trim(body);
  return {true, std::move(out)};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SKILLS_H_
