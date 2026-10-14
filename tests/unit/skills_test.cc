// Copyright 2026 Timon Gentzsch

#include "include/core/skills.h"

#include <sys/stat.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/app/import.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/tools/files.h"
#include "include/tools/registry.h"
#include "include/tools/skill.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestSkillDiscovery() {
  namespace fs = std::filesystem;
  {
    ScopedEnv bundled("UAGENT_SKILL_PATH",
                      std::string(UAGENT_TEST_SOURCE_DIR) + "/skills");
    const auto skills = LoadSkills(UAGENT_TEST_SOURCE_DIR);
    CHECK(!skills.empty());
    bool found_fusion = false, found_import = false;
    for (const Skill& skill : skills) {
      CHECK(!skill.description.empty());
      CHECK(ReadSkillBody(skill).ok);
      if (skill.name == "import") {
        found_import = true;
        CHECK(skill.description.find("only when the user invokes $import") !=
              std::string::npos);
        CHECK(skill.required_tools ==
              std::vector<std::string>({"read_path", "grep", "memory"}));
      }
      if (skill.name == "fusion") {
        found_fusion = true;
        CHECK(skill.description.find("only when the user invokes $fusion") !=
              std::string::npos);
        CHECK(
            skill.required_tools ==
            std::vector<std::string>({"subagent", "read_path", "grep", "run"}));
      }
    }
    CHECK(found_fusion);
    CHECK(found_import);
  }
  TestWorkspace test("skill");
  const fs::path& workspace = test.workspace;
  const fs::path& home = test.home;

  auto write_skill = [](const fs::path& dir, const std::string& text) {
    std::filesystem::create_directories(dir);
    CHECK(ToolWriteFile((dir / "SKILL.md").string(), text)
              .output.starts_with("wrote "));
  };

  CHECK(LoadSkills(workspace).empty());

  // The process-global value is mutated below; retain its original bytes.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const std::string original_executable = ExecutablePath();
  fs::path packaged = test.root / "package";
  fs::path decoy = test.root / "decoy";
  fs::create_directories(packaged / "bin");
  fs::create_directories(decoy / "bin");
  CHECK(ToolWriteFile((packaged / "bin/uagent").string(), "binary")
            .output.starts_with("wrote "));
  CHECK(chmod((packaged / "bin/uagent").c_str(), 0755) == 0);
  CHECK(ToolWriteFile((decoy / "bin/uagent").string(), "not executable")
            .output.starts_with("wrote "));
  write_skill(packaged / "share/uagent/skills/bundled",
              "---\ndescription: packaged with the binary\n---\n\nUse it.\n");
  write_skill(decoy / "share/uagent/skills/decoy",
              "---\ndescription: beside a PATH decoy\n---\n\nIgnore it.\n");
  std::string packaged_path =
      (decoy / "bin").string() + ":" + (packaged / "bin").string();
  ScopedEnv scoped_path("PATH", packaged_path.c_str());
  SetExecutablePath("uagent");
  std::vector<Skill> skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "bundled");
  CHECK(std::none_of(skills.begin(), skills.end(),
                     [](const Skill& skill) { return skill.name == "decoy"; }));
  SetExecutablePath(original_executable);
  fs::remove_all(packaged);
  fs::remove_all(decoy);

  write_skill(home / ".uagent/skills/release",
              "---\nname: release\ndescription: how to cut a release\n"
              "requires-tools: run, grep\nargument-hint: <version>\n---\n\n"
              "Run $ARGUMENTS checks from ${SKILL_DIR}, then tag.\n");
  // No front matter, so nothing to advertise: skipped rather than guessed at.
  write_skill(home / ".uagent/skills/broken", "no front matter here\n");
  skills = LoadSkills(workspace);
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

  // Skills installed for another agent were written for its tools: they are
  // found when its name is listed, and ours wins a name collision.
  write_skill(home / ".claude/skills/vendor-only",
              "---\ndescription: from claude code\n"
              "requires-tools: vendor-tool\n---\n\nVendor body.\n");
  write_skill(home / ".codex/skills/release",
              "---\ndescription: codex's release steps\n---\n\nCodex body.\n");
  CHECK(LoadSkills(workspace).size() == 2);
  setenv("UAGENT_OTHER_AGENTS", "codex", 1);
  CHECK(LoadSkills(workspace).size() == 2);
  setenv("UAGENT_OTHER_AGENTS", "claude, codex", 1);
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
  KeepSupportedSkills(skills, ToolNames(available_tools));
  CHECK(skills.size() == 3);
  available_tools.pop_back();
  KeepSupportedSkills(skills, ToolNames(available_tools));
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

  // The cap keeps the highest-precedence entries: discovery lists the
  // workspace last, so the earliest-found user/vendor skills give way.
  std::vector<Skill> many;
  for (size_t i = 0; i <= kMaxSkills; ++i) {
    many.push_back({"skill-" + std::to_string(i), "d", "", "", {}, ""});
  }
  many = SelectSkills(std::move(many));
  CHECK(many.size() == kMaxSkills);
  CHECK(many.front().name == "skill-1");
  CHECK(many.back().name == "skill-" + std::to_string(kMaxSkills));

  // Discovery descriptions are bounded before the tool returns them.
  write_skill(
      workspace / ".uagent/skills/verbose",
      "---\ndescription: " + std::string(kSkillDescriptionBytes + 1, 'x') +
          "\n---\n\nBody.\n");
  skills = LoadSkills(workspace);
  for (const Skill& s : skills) {
    // The over-long one says so; short ones are left alone.
    CHECK(s.description.ends_with("…") == (s.name == "verbose"));
    CHECK(s.description.size() <= kSkillDescriptionBytes + strlen("…"));
  }
  fs::remove_all(workspace / ".uagent/skills/verbose");

  std::vector<Skill> catalogue_skills;
  catalogue_skills.reserve(64);
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
  Tool tool = SkillTool(skills, {});
  CHECK(tool.name == "skill");
  CHECK(tool.description.find("full workflow supports the requested outcome") !=
        std::string::npos);
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

  // A skill written during the session is not in the catalogue the request
  // advertises, which stays fixed to keep the prompt prefix cacheable. Asking
  // for it by name still has to work, or authoring one would need a restart.
  setenv("UAGENT_SKILL_PATH", (home / ".claude/skills").c_str(), 1);
  Tool frozen = SkillTool(LoadSkills(workspace), {"run"});
  CHECK(!frozen.run({{"name", "just-written"}}, {}).Ok());
  write_skill(home / ".claude/skills/just-written",
              "---\ndescription: authored mid-session\n"
              "requires-tools: run\n---\n\nFresh body.\n");
  ToolResult fresh = frozen.run({{"name", "just-written"}}, {});
  CHECK(fresh.Ok());
  CHECK(fresh.output.find("Fresh body.") != std::string::npos);
  // The rescan honours the same tool requirement as the catalogue did.
  write_skill(home / ".claude/skills/needs-vendor",
              "---\ndescription: authored mid-session\n"
              "requires-tools: vendor-tool\n---\n\nUnusable body.\n");
  CHECK(!frozen.run({{"name", "needs-vendor"}}, {}).Ok());
  unsetenv("UAGENT_SKILL_PATH");
  unsetenv("UAGENT_OTHER_AGENTS");

  Tool crowded = SkillTool(catalogue_skills, {});
  CHECK(crowded.description.size() <= 8000);
  CHECK(crowded.description.find("skill-63") != std::string::npos);
}

}  // namespace uagent

namespace uagent {

void TestImportFromOtherAgents() {
  namespace fs = std::filesystem;
  // Tool references change; prose that happens to use the same words does not.
  const std::string rewritten = RewriteToolReferences(
      "---\ndescription: Demo\nallowed-tools: Bash(ls:*), Read, "
      "Grep, Glob\n---\nRead the file, then run `Bash` or the Edit tool.\n"
      "Use `Task(agent)` for the Write-up; Grep is not a tool here.\n");
  CHECK(rewritten ==
        "---\ndescription: Demo\nallowed-tools: run, read_path, grep\n"
        "requires-tools: edit_file, grep, read_path, run\n---\n"
        "Read the file, then run `run` or the edit_file tool.\n"
        "Use `Task(agent)` for the Write-up; Grep is not a tool here.\n");
  CHECK(RewriteToolReferences("Read the file with Bash.\n") ==
        "Read the file with Bash.\n");
  // Backticks alone prove nothing for a word that is also a class or a verb.
  CHECK(RewriteToolReferences("The Python class `Task` and `Read` it.\n") ==
        "The Python class `Task` and `Read` it.\n");
  CHECK(RewriteToolReferences("Call `exec_command` or the Task tool.\n") ==
        "Call `run` or the subagent tool.\n");
  // Front matter lists come out as the plain comma list the loader reads,
  // whether written quoted, as an array or as a block.
  const std::string lists = RewriteToolReferences(
      "---\ndescription: d\nallowed-tools: \"Bash, Read\"\n"
      "tools: [Bash, \"Read\", Grep]\n"
      "requires-tools:\n  - Bash\n  - Task\n---\nBody\n");
  CHECK(lists ==
        "---\ndescription: d\nallowed-tools: run, read_path\n"
        "tools: run, read_path, grep\n"
        "requires-tools: run, subagent\n---\nBody\n");
  // The added requires-tools line shifts the body by one; every body line
  // stays, the last included (64 lines in all crosses a word of the mask).
  std::string body;
  for (int line = 0; line < 59; ++line) {
    body += "line\n";
  }
  const std::string block = RewriteToolReferences(
      "---\nallowed-tools:\n  - Bash\n---\n" + body + "last");
  CHECK(block ==
        "---\nallowed-tools: run\nrequires-tools: run\n---\n" + body + "last");
  std::istringstream loaded(RewriteToolReferences(
      "---\ndescription: d\nrequires-tools: [Bash, Read]\n---\n"));
  std::vector<std::string> required;
  ParseSkillFrontMatter(loaded, nullptr, &required);
  CHECK(required == std::vector<std::string>({"run", "read_path"}));

  TestWorkspace test("import");
  auto write = [](const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  };
  const fs::path skills = test.home / ".claude/skills";
  const std::string manifest = "---\ndescription: Demo\n---\nUse `Bash`.\n";
  write(skills / "synced/id/demo/SKILL.md", manifest);
  write(skills / "synced/id/demo/data.bin", std::string("\0\1\2", 3));
  write(skills / "synced/id/walled/SKILL.md", manifest);
  write(skills / "synced/id/walled/LICENSE.txt",
        "Users may not: Reproduce or copy these materials.");
  write(skills / ".system/hidden/SKILL.md", manifest);
  write(test.root / "outside.txt", "secret");
  write(skills / "linked/SKILL.md", manifest);
  fs::create_symlink(test.root / "outside.txt", skills / "linked/outside.txt");
  write(skills / "a/b/c/d/e/deep/SKILL.md", manifest);

  json listed = ImportControl({{"action", "list"}}, test.workspace);
  CHECK(listed["items"].size() == 3);
  for (const json& item : listed["items"]) {
    const bool blocked = item["name"] != "demo";
    CHECK(item["kind"] == "skill" && item["source"] == "claude");
    CHECK(item.contains("blocked") == blocked);
    CHECK(!item["collides"].get<bool>());
  }

  json done =
      ImportControl({{"action", "run"}, {"key", "all"}}, test.workspace);
  CHECK(done["imported"].size() == 1 && done["skipped"].size() == 2);
  const fs::path copy = test.home / ".uagent/skills/demo";
  std::string text;
  std::getline(std::ifstream(copy / "SKILL.md"), text, '\0');
  CHECK(text.find("requires-tools: run") != std::string::npos);
  CHECK(fs::exists(copy) && fs::is_directory(copy));
  std::ifstream data(copy / "data.bin", std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(data)), {});
  CHECK(bytes == std::string("\0\1\2", 3));
  CHECK(!fs::exists(test.home / ".uagent/skills/walled"));
  CHECK(!fs::exists(test.home / ".uagent/skills/linked"));
  CHECK(ImportControl({{"action", "run"}, {"key", "demo"}}, test.workspace)
            .contains("error"));
}

}  // namespace uagent

namespace uagent {

namespace {
std::string BlockedReason(const json& listed, const std::string& name) {
  for (const json& item : listed["items"]) {
    if (item["name"] == name) return JsonValue(item, "blocked", "");
  }
  return "missing";
}
}  // namespace

void TestImportBoundaries() {
  namespace fs = std::filesystem;
  TestWorkspace test("import-bounds");
  auto write = [](const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  };
  const fs::path skills = test.home / ".claude/skills";
  const std::string manifest = "---\ndescription: Demo\n---\nBody\n";
  // Hidden files travel with a skill; its repository metadata does not.
  write(skills / "plain/SKILL.md", manifest);
  write(skills / "plain/.env.example", "KEY=");
  write(skills / "plain/.hidden/note.txt", "kept");
  write(skills / "plain/.git/config", "dropped");
  // A license that wraps its wording, one that a field names, and COPYING.
  write(skills / "wrapped/SKILL.md", manifest);
  write(skills / "wrapped/LICENSE", "You may not\n  COPY these files.");
  write(skills / "named/SKILL.md",
        "---\ndescription: d\nlicense: Proprietary. TERMS.txt has the "
        "terms\n---\n");
  write(skills / "named/TERMS.txt", "No copying: you may not copy this.");
  write(skills / "copying/SKILL.md", manifest);
  write(skills / "copying/COPYING", "Users may not redistribute this.");
  // Terms written in the field, a file path in it, and a path that is missing.
  write(skills / "inline/SKILL.md",
        "---\ndescription: d\nlicense: You may not copy this skill\n---\n");
  write(skills / "pathed/SKILL.md",
        "---\ndescription: d\nlicense: See legal/TERMS.txt\n---\n");
  write(skills / "pathed/legal/TERMS.txt", "Redistribution is not permitted.");
  write(skills / "absent/SKILL.md",
        "---\ndescription: d\nlicense: See legal/TERMS.txt\n---\n");
  // Links: to a folder, out of the folder, and a skill folder that is one.
  write(test.root / "elsewhere/data.txt", "x");
  write(skills / "folderlink/SKILL.md", manifest);
  fs::create_directories(skills / "folderlink/real");
  fs::create_directory_symlink(skills / "folderlink/real",
                               skills / "folderlink/alias");
  write(test.root / "outside-skill/SKILL.md", manifest);
  fs::create_directory_symlink(test.root / "outside-skill", skills / "escaped");
  write(skills / "pipe/SKILL.md", manifest);
  CHECK(mkfifo((skills / "pipe/queue").c_str(), 0600) == 0);

  json listed =
      ImportControl({{"action", "list"}, {"only", "skill"}}, test.workspace);
  CHECK(BlockedReason(listed, "plain").empty());
  CHECK(BlockedReason(listed, "wrapped").find("license") != std::string::npos);
  CHECK(BlockedReason(listed, "named").find("license") != std::string::npos);
  CHECK(BlockedReason(listed, "copying").find("license") != std::string::npos);
  CHECK(BlockedReason(listed, "inline").find("license") != std::string::npos);
  CHECK(BlockedReason(listed, "pathed").find("license") != std::string::npos);
  CHECK(BlockedReason(listed, "absent").find("not found") != std::string::npos);
  CHECK(BlockedReason(listed, "folderlink").find("alias") != std::string::npos);
  CHECK(BlockedReason(listed, "escaped").find("link out of") !=
        std::string::npos);
  CHECK(BlockedReason(listed, "pipe").find("queue") != std::string::npos);

  // An existing folder is never adopted, and what it holds is left alone.
  const fs::path managed = test.home / ".uagent/skills";
  write(managed / "plain/mine.txt", "mine");
  json refused =
      ImportControl({{"action", "run"}, {"key", "plain"}}, test.workspace);
  CHECK(refused.contains("error"));
  CHECK(fs::exists(managed / "plain/mine.txt"));
  CHECK(!fs::exists(managed / "plain/SKILL.md"));
  fs::remove_all(managed / "plain");
  CHECK(!ImportControl({{"action", "run"}, {"key", "plain"}}, test.workspace)
             .contains("error"));
  CHECK(fs::exists(managed / "plain/.env.example"));
  CHECK(fs::exists(managed / "plain/.hidden/note.txt"));
  CHECK(!fs::exists(managed / "plain/.git"));
  CHECK(fs::is_empty(test.home / ".uagent/staging"));
  for (const auto& entry : fs::directory_iterator(managed)) {
    CHECK(!entry.path().filename().string().starts_with("."));
  }

  // A failed copy removes only its own staging folder.
  CHECK(ImportControl({{"action", "run"}, {"key", "pipe"}}, test.workspace)
            .contains("error"));
  CHECK(fs::is_empty(test.home / ".uagent/staging"));
  for (const auto& entry : fs::directory_iterator(managed)) {
    CHECK(!entry.path().filename().string().starts_with("."));
  }

  // The skills folder itself must not be a link.
  fs::remove_all(managed);
  fs::create_directories(test.root / "stolen");
  fs::create_directories(test.home / ".uagent");
  fs::create_directory_symlink(test.root / "stolen", managed);
  CHECK(ImportControl({{"action", "run"}, {"key", "plain"}}, test.workspace)
            .contains("error"));
  CHECK(fs::is_empty(test.root / "stolen"));
  fs::remove(managed);

  // Only the sources asked for are walked, and a listing is bounded.
  CHECK(ImportControl({{"action", "list"}, {"only", "memory"}},
                      test.workspace)["items"]
            .empty());
  for (int index = 0; index < 600; ++index) {
    write(skills / ("bulk/s" + std::to_string(index)) / "SKILL.md", manifest);
  }
  listed = ImportControl({{"action", "list"}}, test.workspace);
  CHECK(listed["items"].size() == 512);
  CHECK(listed["truncated"].get<bool>());
  CHECK(listed.dump().size() < size_t{1024} * 1024);
}

}  // namespace uagent

namespace uagent {

void TestImportTruncation() {
  namespace fs = std::filesystem;
  TestWorkspace test("import-truncation");
  const fs::path skills = test.home / ".claude/skills";
  for (int index = 0; index < 520; ++index) {
    const fs::path dir = skills / ("s" + std::to_string(index));
    fs::create_directories(dir);
    std::ofstream(dir / "SKILL.md") << "---\ndescription: d\n---\nBody\n";
  }
  // A named skill is found even past the listing's cap.
  json named =
      ImportControl({{"action", "run"}, {"key", "s99"}}, test.workspace);
  CHECK(named["imported"].size() == 1);
  // "all" says it stopped short, and a repeat reaches what it left.
  json first =
      ImportControl({{"action", "run"}, {"key", "all"}}, test.workspace);
  CHECK(first["imported"].size() == 512);
  CHECK(first["truncated"].get<bool>());
  json second =
      ImportControl({{"action", "run"}, {"key", "all"}}, test.workspace);
  CHECK(second["imported"].size() == 7);
  CHECK(!second["truncated"].get<bool>());
}

}  // namespace uagent
