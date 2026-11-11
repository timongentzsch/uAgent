// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SKILLS_H_
#define UAGENT_INCLUDE_CORE_SKILLS_H_
// Skill discovery. A skill is a directory holding SKILL.md; only its front
// matter is read until the model selects the skill.

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

// `key: value` pairs between the opening and closing `---`: scalar metadata
// and a comma-separated tool list. Other keys are ignored.
void ParseSkillFrontMatter(std::istream& input,
                           std::string* description = nullptr,
                           std::vector<std::string>* required_tools = nullptr,
                           std::string* argument_hint = nullptr);

// Where µAgent's own data named `name` is installed: share/uagent/<name>
// beside the binary (a build tree) or beside its bin folder (an install).
std::filesystem::path InstalledShare(std::string_view name);

// User-level paths first, then the folder's, µAgent's own last in each group;
// a later path wins. UAGENT_SKILL_PATH replaces the whole list.
std::vector<std::filesystem::path> SkillSearchPath(
    const std::filesystem::path& cwd);

bool SkillExcluded(const std::string& name);
// Whether `agent` ("claude" or "codex") is one whose skills and memories are
// read too (UAGENT_OTHER_AGENTS); none is by default.
bool ReadsAgent(std::string_view agent);

std::vector<Skill> DiscoverSkills(const std::filesystem::path& cwd);
std::vector<Skill> SelectSkills(std::vector<Skill> discovered);
std::vector<Skill> LoadSkills(const std::filesystem::path& cwd);

// The complete body without its front matter. An oversized skill fails
// rather than hand the model a partial procedure.
SkillReadResult ReadSkillBody(const Skill& skill,
                              const std::string& arguments = {});

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SKILLS_H_
