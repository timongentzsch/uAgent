// Copyright 2026 Timon Gentzsch

#include <string>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

void TestSkillDiscovery() {
  namespace fs = std::filesystem;
  fs::path original = fs::current_path();
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-skill-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::path workspace = root / "workspace";
  fs::path home = root / "home";
  fs::create_directories(workspace);
  fs::create_directories(home);
  const char* prior_home_value = getenv("HOME");
  std::string prior_home = prior_home_value ? prior_home_value : "";
  bool had_home = prior_home_value != nullptr;
  setenv("HOME", home.c_str(), 1);
  fs::current_path(workspace);
  workspace = fs::current_path();

  auto write_skill = [](const fs::path& dir, const std::string& text) {
    std::filesystem::create_directories(dir);
    CHECK(ToolWriteFile((dir / "SKILL.md").string(), text)
              .output.starts_with("wrote "));
  };

  CHECK(LoadSkills(workspace).empty());

  write_skill(home / ".uagent/skills/release",
              "---\nname: release\ndescription: how to cut a release\n---\n\n"
              "Run the checks, then tag.\n");
  // No front matter, so nothing to advertise: skipped rather than guessed at.
  write_skill(home / ".uagent/skills/broken", "no front matter here\n");
  std::vector<Skill> skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "release");
  CHECK(skills[0].description == "how to cut a release");
  CHECK(skills[0].dir == (home / ".uagent/skills/release").string());

  // The body arrives without its front matter and names its own directory, so
  // relative references inside it resolve.
  SkillReadResult body = ReadSkillBody(skills[0]);
  CHECK(body.ok);
  CHECK(body.output.find("Run the checks, then tag.") != std::string::npos);
  CHECK(body.output.find("description:") == std::string::npos);
  CHECK(body.output.find(skills[0].dir) != std::string::npos);

  // A workspace skill of the same name shadows the global one.
  write_skill(
      workspace / ".uagent/skills/release",
      "---\nname: whatever\ndescription: this repo's release steps\n---\n\n"
      "Use the makefile.\n");
  write_skill(workspace / ".uagent/skills/lint",
              "---\ndescription: how this repo lints\n---\n\nRun ruff.\n");
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 2);
  const Skill* release = nullptr;
  const Skill* lint = nullptr;
  for (const Skill& s : skills) {
    if (s.name == "release") release = &s;
    if (s.name == "lint") lint = &s;
  }
  CHECK(release != nullptr);
  CHECK(lint != nullptr);
  if (release) {
    CHECK(release->description == "this repo's release steps");
    CHECK(release->dir == (workspace / ".uagent/skills/release").string());
  }
  // The directory name is authoritative even when front matter disagrees or
  // omits it, so a skill can never claim another skill's name.
  if (lint) CHECK(lint->description == "how this repo lints");

  // Skills installed for another agent are already on the machine and use the
  // same format, so they are found too; ours wins a name collision.
  write_skill(home / ".claude/skills/vendor-only",
              "---\ndescription: from claude code\n---\n\nVendor body.\n");
  write_skill(home / ".codex/skills/release",
              "---\ndescription: codex's release steps\n---\n\nCodex body.\n");
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 3);
  for (const Skill& s : skills) {
    // The workspace copy still outranks both the user's and the vendors'.
    if (s.name == "release") {
      CHECK(s.description == "this repo's release steps");
    }
  }

  // An explicit path replaces the defaults outright, so a user can narrow the
  // catalogue to exactly what they want to pay for.
  setenv("UAGENT_SKILL_PATH", (home / ".claude/skills").c_str(), 1);
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "vendor-only");
  unsetenv("UAGENT_SKILL_PATH");

  // Individual skills can be hidden without replacing every discovery root.
  setenv("UAGENT_SKILL_EXCLUDE", "vendor-only, missing", 1);
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 2);
  CHECK(std::none_of(skills.begin(), skills.end(), [](const Skill& skill) {
    return skill.name == "vendor-only";
  }));
  unsetenv("UAGENT_SKILL_EXCLUDE");

  // The cap keeps the highest-precedence entries rather than filling up on
  // user/vendor skills before the workspace is scanned.
  setenv("UAGENT_SKILLS", "1", 1);
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "release");
  CHECK(skills[0].dir == (workspace / ".uagent/skills/release").string());
  unsetenv("UAGENT_SKILLS");

  // Descriptions ride every request, so an over-long one is truncated rather
  // than dropping the skill. The cap bounds the text; the ellipsis marking the
  // cut is allowed on top of it.
  setenv("UAGENT_SKILL_DESC_BYTES", "16", 1);
  skills = LoadSkills(workspace);
  int64_t marked = 0;
  for (const Skill& s : skills) {
    CHECK(s.description.size() <= 16 + strlen("…"));
    marked += s.description.ends_with("…");
  }
  CHECK(marked > 0);  // the over-long ones say so; short ones are left alone
  unsetenv("UAGENT_SKILL_DESC_BYTES");

  // The tool advertises every skill by name and returns the one asked for.
  skills = LoadSkills(workspace);
  Tool tool = SkillTool(skills);
  CHECK(tool.name == "skill");
  CHECK(!tool.mutating);
  json names = tool.parameters["properties"]["name"]["enum"];
  CHECK(names.size() == 3);
  CHECK(tool.run({{"name", "lint"}}, {}).output.find("Run ruff.") !=
        std::string::npos);
  ToolResult missing_skill = tool.run({{"name", "nope"}}, {});
  CHECK(!missing_skill.Ok());
  CHECK(missing_skill.error == ToolErrorCode::kNotFound);
  CHECK(missing_skill.output.starts_with("error:"));

  fs::current_path(original);
  if (had_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
