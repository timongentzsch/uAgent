// Copyright 2026 Timon Gentzsch
#include "include/agent/prompt.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "include/agent/agent.h"
#include "include/core/env.h"
#include "include/core/project.h"
#include "include/core/runtime_config.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {
namespace fs = std::filesystem;
}  // namespace

// The system message is the base, then the context layers, each source kept
// for inspection; nothing replaces the base.
void TestResolvedPrompt() {
  const json context = {{{"scope", "repository"}, {"text", "project rules"}}};
  const json resolved = ResolvePrompt("base", context);
  CHECK(JsonValue(resolved, "effective", "") == "base\n\nproject rules");
  CHECK(resolved["sources"].size() == 2);
  CHECK(ResolvePrompt(std::string(kSystemPromptBytes + 1, 'x'))
            .contains("error"));
}

// Two files a person edits: theirs and the folder's.
void TestInstructionFiles() {
  TestWorkspace workspace("instruction-files");
  const auto cwd = workspace.workspace;
  CHECK(InstructionPath(false, cwd).filename() == "AGENTS.md");
  CHECK(WriteInstructionFile(true, cwd, "Run ctest.").empty());
  CHECK(WriteInstructionFile(false, cwd, "Keep agents small.").empty());
  struct stat info{};
  CHECK(stat(InstructionPath(true, cwd).c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0644);
  CHECK(stat(InstructionPath(false, cwd).c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0600);
  const auto session = LoadProjectInstructions(cwd, ProjectDocBytes());
  CHECK(session.text.find("Keep agents small.") <
        session.text.find("Run ctest."));
  const json shown = InstructionFiles(cwd);
  CHECK(shown["files"].size() == 2);
  CHECK(shown["files"][0]["text"] == "Keep agents small.");
  CHECK(shown["files"][1]["text"] == "Run ctest.");
  CHECK(WriteInstructionFile(false, cwd,
                             std::string(ProjectDocBytes() + 1, 'x')) != "");
  // Two editors from the same text: the second save is refused, not lost.
  CHECK(WriteInstructionFile(true, cwd, "Mine.", "Run ctest.").empty());
  CHECK(WriteInstructionFile(true, cwd, "Theirs.", "Run ctest.")
            .find("changed since") != std::string::npos);
  CHECK(ReadInstructionFile(InstructionPath(true, cwd)) == "Mine.");
  // The editor edits the file the loader reads at that level, and never
  // writes through a link.
  {
    std::ofstream(cwd / "CLAUDE.md") << "Legacy.";
  }
  fs::remove(cwd / "AGENTS.md");
  CHECK(InstructionPath(true, cwd).filename() == "AGENTS.md");
  setenv("UAGENT_OTHER_AGENTS", "claude", 1);
  CHECK(InstructionPath(true, cwd).filename() == "CLAUDE.md");
  CHECK(InstructionFiles(cwd)["files"][1]["text"] == "Legacy.");
  unsetenv("UAGENT_OTHER_AGENTS");
  fs::remove(cwd / "CLAUDE.md");
  fs::create_symlink(cwd / "elsewhere", cwd / "AGENTS.md");
  CHECK(WriteInstructionFile(true, cwd, "x").find("symbolic link") !=
        std::string::npos);
  CHECK(!fs::exists(cwd / "elsewhere"));
}

// What /instructions previews is what the request sends, before and after a
// save and load.
void TestPromptRequestParity() {
  TestWorkspace workspace("prompt-request");
  Api api{RuntimeConfig{}};
  std::vector<Tool> tools;
  ActivitySupervisor processes;
  UsageAccumulator usage;
  Agent agent(
      api, tools, processes, usage,
      [](const Tool&, const json&, int64_t) { return std::string("denied"); });
  const std::string effective = agent.PromptPreview()["effective"];
  agent.PreviewContext();
  CHECK(agent.ModelRequest()["messages"][0]["content"] == effective);
  const auto stable = agent.ModelRequest();
  agent.PreviewContext();
  CHECK(agent.ModelRequest() == stable);
  const auto path = (workspace.workspace / "session.json").string();
  std::string error;
  CHECK(agent.Save(path, error));
  CHECK(agent.Load(path, workspace.workspace.string(), error));
  CHECK(agent.PromptPreview()["effective"] == effective);
}
}  // namespace uagent
