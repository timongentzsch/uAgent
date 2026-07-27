// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SKILLS_H_
#define UAGENT_INCLUDE_CORE_SKILLS_H_
// Skill discovery. A skill is a directory holding SKILL.md: front matter
// naming it, then the procedure itself. Only the front matter is read at
// startup — the body arrives when the model asks for it, so owning many
// skills costs one line of schema each rather than one document each.

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
  std::string description;  // front-matter summary; rides every request
  std::string dir;          // absolute, so SKILL.md can reference siblings
  std::string path;         // the SKILL.md itself
};

inline int64_t SkillBodyBytes() {
  return std::max(int64_t{1024}, EnvLong("UAGENT_SKILL_BYTES", 16 * 1024));
}

// Descriptions are sent with every request, so they are bounded far more
// tightly than bodies, which are sent only when a skill is actually used.
inline int64_t SkillDescriptionBytes() {
  return std::max(int64_t{16}, EnvLong("UAGENT_SKILL_DESC_BYTES", 512));
}

inline int64_t MaxSkills() {
  return std::max(int64_t{1}, EnvLong("UAGENT_SKILLS", 64));
}

// `key: value` pairs between the opening and closing `---`. Enough YAML for
// the two keys a skill declares; anything else in the block is ignored.
inline void ParseSkillFrontMatter(std::istream& input, std::string& name,
                                  std::string& description) {
  std::string line;
  if (!std::getline(input, line) || Trim(line) != "---") return;
  while (std::getline(input, line)) {
    if (Trim(line) == "---") return;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = Trim(line.substr(0, colon));
    std::string value = Unquote(Trim(line.substr(colon + 1)));
    if (key == "name") name = std::move(value);
    if (key == "description") description = std::move(value);
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
    for (size_t start = 0; start <= custom.size();) {
      size_t sep = custom.find(':', start);
      std::string entry = Trim(custom.substr(
          start, sep == std::string::npos ? std::string::npos : sep - start));
      if (!entry.empty()) path.push_back(fs::path(entry));
      if (sep == std::string::npos) break;
      start = sep + 1;
    }
    return path;
  }
  // ".agents" is the vendor-neutral location; the others are where Claude Code
  // and Codex keep theirs.
  const char* kVendors[] = {".agents", ".claude", ".codex"};
  std::string home = UserHome();
  if (!home.empty()) {
    for (const char* vendor : kVendors) {
      path.push_back(fs::path(home) / vendor / "skills");
    }
  }
  path.push_back(fs::path(GlobalBase()) / "skills");
  for (const char* vendor : kVendors) path.push_back(cwd / vendor / "skills");
  path.push_back(cwd / ".uagent" / "skills");
  return path;
}

inline std::vector<Skill> LoadSkills(const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  std::vector<Skill> found;
  auto scan = [&](const fs::path& base) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(base, ec), end; it != end && !ec;
         it.increment(ec)) {
      if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& dir : dirs) {
      fs::path file = dir / "SKILL.md";
      std::ifstream input(file);
      if (!input) continue;
      std::string name, description;
      ParseSkillFrontMatter(input, name, description);
      // The directory name wins: it is what the model names, and it cannot
      // collide with another skill or carry a path separator.
      name = SafeFileComponent(dir.filename().string());
      if (Trim(description).empty()) continue;
      description = Utf8Trunc(std::move(description),
                              static_cast<size_t>(SkillDescriptionBytes()));
      Skill skill{name, description, dir.string(), file.string()};
      auto same = std::find_if(found.begin(), found.end(), [&](const Skill& s) {
        return s.name == skill.name;
      });
      if (same != found.end()) {
        *same = std::move(skill);
      } else if (static_cast<int64_t>(found.size()) < MaxSkills()) {
        found.push_back(std::move(skill));
      }
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

// The body without its front matter, bounded. The directory is prepended so
// relative references inside the skill resolve for read_file and run.
inline std::string ReadSkillBody(const Skill& skill) {
  std::ifstream input(skill.path, std::ios::binary);
  if (!input) return "error: cannot read " + skill.path;
  std::string name, description;
  ParseSkillFrontMatter(input, name, description);
  size_t cap = static_cast<size_t>(SkillBodyBytes());
  std::string body(cap + 1, '\0');
  input.read(body.data(), static_cast<std::streamsize>(body.size()));
  size_t read = static_cast<size_t>(input.gcount());
  bool truncated = read > cap;
  body.resize(std::min(read, cap));
  body = Utf8Prefix(std::move(body), cap);
  if (Trim(body).empty()) return "error: " + skill.path + " has no body";
  std::string out =
      "[skill " + skill.name + " — files in " + skill.dir + "]\n" + Trim(body);
  if (truncated) out += "\n[truncated; raise UAGENT_SKILL_BYTES]";
  return out;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SKILLS_H_
