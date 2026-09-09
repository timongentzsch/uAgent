// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SKILLS_H_
#define UAGENT_INCLUDE_CORE_SKILLS_H_
// Skill discovery. A skill is a directory holding SKILL.md: front matter
// naming it, then the procedure itself. Only the front matter is read at
// startup. Discovery is deferred behind one fixed tool schema, and the body
// arrives only when the model selects a skill.

#include <filesystem>
#include <istream>
#include <string>
#include <vector>

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
void ParseSkillFrontMatter(std::istream& input,
                           std::string* description = nullptr,
                           std::vector<std::string>* required_tools = nullptr,
                           std::string* argument_hint = nullptr);

std::filesystem::path InstalledSkillsPath();

// SKILL.md is an open format that ~30 agents read from their own directory, so
// a skill installed for any of them is already on the machine and usable here.
// User-level paths first, then the workspace's, and ours last in each group:
// later wins, so a project overrides a user skill and µAgent's own overrides a
// vendor copy of the same name. The release-installed tree outranks the mutable
// user directory because it is the only copy guaranteed to match this binary.
// UAGENT_SKILL_PATH replaces the whole list.
std::vector<std::filesystem::path> SkillSearchPath(
    const std::filesystem::path& cwd);

bool SkillExcluded(const std::string& name);

std::vector<Skill> DiscoverSkills(const std::filesystem::path& cwd);
std::vector<Skill> SelectSkills(std::vector<Skill> discovered);
std::vector<Skill> LoadSkills(const std::filesystem::path& cwd);

// The complete body without its front matter, bounded. Oversized skills fail
// explicitly instead of silently giving the model a partial procedure.
SkillReadResult ReadSkillBody(const Skill& skill,
                              const std::string& arguments = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SKILLS_H_
