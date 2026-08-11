// Copyright 2026 Timon Gentzsch

#include <string>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

void TestSkillDiscovery() {
  namespace fs = std::filesystem;
  TestWorkspace test("skill");
  const fs::path& workspace = test.workspace;
  const fs::path& home = test.home;

  auto write_skill = [](const fs::path& dir, const std::string& text) {
    std::filesystem::create_directories(dir);
    CHECK(ToolWriteFile((dir / "SKILL.md").string(), text)
              .output.starts_with("wrote "));
  };

  CHECK(LoadSkills(workspace).empty());

  write_skill(home / ".uagent/skills/release",
              "---\nname: release\ndescription: how to cut a release\n"
              "requires-tools: run, grep\nargument-hint: <version>\n---\n\n"
              "Run $ARGUMENTS checks from ${SKILL_DIR}, then tag.\n");
  // No front matter, so nothing to advertise: skipped rather than guessed at.
  write_skill(home / ".uagent/skills/broken", "no front matter here\n");
  std::vector<Skill> skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "release");
  CHECK(skills[0].description == "how to cut a release");
  CHECK(skills[0].required_tools == std::vector<std::string>({"run", "grep"}));
  CHECK(skills[0].argument_hint == "<version>");
  CHECK(skills[0].dir == (home / ".uagent/skills/release").string());

  // The body arrives without its front matter and names its own directory, so
  // relative references inside it resolve.
  SkillReadResult body = ReadSkillBody(skills[0]);
  CHECK(body.ok);
  CHECK(body.output.find("Run  checks from " + skills[0].dir) !=
        std::string::npos);
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
              "---\ndescription: from claude code\n"
              "requires-tools: vendor-tool\n---\n\nVendor body.\n");
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
  SkillReadResult vendor_body = ReadSkillBody(skills[0]);
  CHECK(vendor_body.ok);
  CHECK(vendor_body.output.find("cross-agent compatibility") !=
        std::string::npos);
  CHECK(vendor_body.output.find("currently registered in µAgent") !=
        std::string::npos);
  unsetenv("UAGENT_SKILL_PATH");

  skills = LoadSkills(workspace);
  std::vector<Tool> available_tools = {
      MakeTool("run", "", json::object(),
               [](const json&, const ToolContext&) { return ToolSuccess(""); }),
      MakeTool(
          "vendor-tool", "", json::object(),
          [](const json&, const ToolContext&) { return ToolSuccess(""); })};
  KeepSupportedSkills(skills, available_tools);
  CHECK(skills.size() == 3);
  available_tools.pop_back();
  KeepSupportedSkills(skills, available_tools);
  CHECK(std::none_of(skills.begin(), skills.end(), [](const Skill& skill) {
    return skill.name == "vendor-only";
  }));

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

  // Discovery descriptions are bounded before the tool returns them.
  setenv("UAGENT_SKILL_DESC_BYTES", "16", 1);
  skills = LoadSkills(workspace);
  int64_t marked = 0;
  for (const Skill& s : skills) {
    CHECK(s.description.size() <= 16 + strlen("…"));
    marked += s.description.ends_with("…");
  }
  CHECK(marked > 0);  // the over-long ones say so; short ones are left alone
  unsetenv("UAGENT_SKILL_DESC_BYTES");

  std::vector<Skill> catalogue_skills;
  for (int i = 0; i < 64; ++i) {
    catalogue_skills.push_back(
        {"skill-" + std::to_string(i), std::string(512, 'x'), "", "", {}, ""});
  }
  std::string catalogue = SkillCatalogue(catalogue_skills, "");
  CHECK(catalogue.size() < 8000);
  CHECK(catalogue.find("skill-63") != std::string::npos);
  CHECK(catalogue.find(':') == std::string::npos);

  // Like Codex and OpenCode, the model sees bounded metadata up front while
  // exact selection still defers the full body.
  skills = LoadSkills(workspace);
  Tool tool = SkillTool(skills);
  CHECK(tool.name == "skill");
  CHECK(!ToolParameters(tool).at("properties").contains("timeout"));
  CHECK(!tool.mutating);
  CHECK(!tool.parameters["properties"]["name"].contains("enum"));
  CHECK(tool.description.find("lint: how this repo lints") !=
        std::string::npos);
  CHECK(tool.description.find("release: this repo's release steps") !=
        std::string::npos);
  CHECK(tool.description.size() <= 8000);
  CHECK(tool.description.find("Run ruff.") == std::string::npos);
  CHECK(JsonDump(tool.parameters).find("Run ruff.") == std::string::npos);
  CHECK(tool.run({{"name", "lint"}}, {}).output.find("Run ruff.") !=
        std::string::npos);
  ToolResult arguments =
      tool.run({{"name", "release"}, {"arguments", "v1.2.3"}}, {});
  CHECK(arguments.output.find("v1.2.3") != std::string::npos);
  CHECK(tool.run({{"query", "lint"}}, {}).output.find("Run ruff.") !=
        std::string::npos);
  CHECK(tool.run(json::object(), {}).output.find("release") !=
        std::string::npos);
  CHECK(!tool.run({{"query", "missing topic"}}, {}).Ok());
  ToolResult missing_skill = tool.run({{"name", "nope"}}, {});
  CHECK(!missing_skill.Ok());
  CHECK(missing_skill.error == ToolErrorCode::kNotFound);
  CHECK(missing_skill.output.starts_with("error:"));
  ToolResult misspelled = tool.run({{"name", "vendor_only"}}, {});
  CHECK(!misspelled.Ok());
  CHECK(misspelled.output.find("did you mean vendor-only?") !=
        std::string::npos);

  Tool crowded = SkillTool(catalogue_skills);
  CHECK(crowded.description.size() <= 8000);
  CHECK(crowded.description.find("skill-63") != std::string::npos);
}

}  // namespace uagent
