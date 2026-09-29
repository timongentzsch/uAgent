// Copyright 2026 Timon Gentzsch
#include "include/agent/prompt.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "include/agent.h"
#include "include/core/project.h"
#include "include/tools/adapt_system.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {
namespace fs = std::filesystem;
struct Harness {
  AdaptiveSystemState state;
  Api api{RuntimeConfig{}};
  std::vector<Tool> tools;
  ProcessSupervisor processes;
  UsageAccumulator usage;
  Agent agent{api,   tools, processes, usage,
              [](const Tool&, const json&, int64_t) { return false; },
              {},    {},    {},        &state};
  json Set(const std::string& mode, const std::string& text) {
    return agent.SelfDirective({{"action", "set"},
                                {"mode", mode},
                                {"text", text},
                                {"revision", Revision()}});
  }
  std::string Revision() {
    return agent.SelfDirective({})["item"]["revision"];
  }
};
}  // namespace

// The self-directive overlays or replaces the base for one conversation;
// context layers (host facts, instructions) always follow it.
void TestSelfDirective() {
  TestWorkspace workspace("self-directive");
  const json context = {{{"scope", "runtime"}, {"text", "host facts"}}};
  AdaptiveSystemState self{.instructions = "focus", .mode = "overlay"};
  CHECK(ResolvePrompt("base", &self, context)["effective"] ==
        "base\n\nfocus\n\nhost facts");
  self.mode = "replace";
  CHECK(ResolvePrompt("base", &self, context)["effective"] ==
        "focus\n\nhost facts");
  CHECK(ResolvePrompt("base", nullptr, context)["effective"] ==
        "base\n\nhost facts");

  Harness h;
  const std::string base = h.agent.PromptPreview()["effective"];
  CHECK(h.Set("overlay", "unique value")["effective"].get<std::string>().find(
            "unique value") != std::string::npos);
  h.agent.SelfDirective({{"action", "edit"},
                         {"old", "unique"},
                         {"new", "edited"},
                         {"revision", h.Revision()}});
  CHECK(h.state.instructions == "edited value");
  CHECK(h.agent.SelfDirective({{"action", "edit"},
                               {"old", "missing"},
                               {"new", "x"},
                               {"revision", h.Revision()}})
            .contains("error"));
  // A stale revision is a conflict, never a silent overwrite.
  CHECK(h.agent.SelfDirective({{"action", "set"},
                               {"text", "stale"},
                               {"revision", "0"}})["conflict"] == true);
  CHECK(h.Set("overlay", std::string(kAdaptiveSystemBytes + 1, 'x'))
            .contains("error"));
  h.agent.SelfDirective({{"action", "preview"},
                         {"mode", "replace"},
                         {"text", "only this"},
                         {"revision", h.Revision()}});
  CHECK(h.state.instructions == "edited value");  // preview commits nothing
  CHECK(h.agent.SelfDirective({{"action", "reset"},
                               {"revision", h.Revision()}})
            .contains("effective"));
  CHECK(h.agent.PromptPreview()["effective"] == base);

  // Replacing the base needs the user, once, for exactly what they saw.
  auto tool = AdaptSystemTool(h.state, [&h](const json& request) {
    return h.agent.SelfDirective(request);
  });
  json args = {{"action", "set"},
               {"mode", "replace"},
               {"text", "approved"},
               {"revision", h.Revision()},
               {"reason", "explicit user request"}};
  CHECK(RequiredApproval(tool, args) == ApprovalClass::kMandatoryHuman);
  CHECK(!tool.run(args, {}).Ok());
  CHECK(tool.approval_preview(args).find("approved") != std::string::npos);
  CHECK(tool.run(args, {}).Ok());
  CHECK(h.state.mode == "replace" && h.state.instructions == "approved");
  CHECK(!tool.run(args, {}).Ok());  // single-use approval
  args["revision"] = h.Revision();
  args["text"] = "second";
  tool.approval_preview(args);
  h.Set("replace", "changed during approval");
  CHECK(!tool.run(args, {}).Ok());
}

// Four files a person edits; a coordinator reads its own after AGENTS.md.
void TestInstructionFiles() {
  TestWorkspace workspace("instruction-files");
  const auto cwd = workspace.workspace;
  CHECK(InstructionPath(false, false, cwd).filename() == "AGENTS.md");
  CHECK(InstructionPath(true, true, cwd) ==
        cwd / ".uagent" / "COORDINATOR.md");
  CHECK(WriteInstructionFile(false, true, cwd, "Run ctest.").empty());
  CHECK(WriteInstructionFile(true, false, cwd, "Keep threads small.").empty());
  struct stat info{};
  CHECK(stat(InstructionPath(false, true, cwd).c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0644);
  CHECK(stat(InstructionPath(true, false, cwd).c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0600);
  CHECK(WriteInstructionFile(true, true, cwd, "Folder rule.").empty());
  CHECK(stat(InstructionPath(true, true, cwd).c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0644);
  const auto session = LoadProjectInstructions(cwd, kProjectDocBytes);
  CHECK(session.text.find("Run ctest.") != std::string::npos);
  CHECK(session.text.find("Keep threads small.") == std::string::npos);
  const auto coordinator = LoadProjectInstructions(cwd, kProjectDocBytes, true);
  CHECK(coordinator.text.find("Run ctest.") <
        coordinator.text.find("Keep threads small."));
  const json shown = InstructionFiles(cwd);
  CHECK(shown["files"].size() == 4);
  CHECK(shown["files"][1]["text"] == "Run ctest.");
  CHECK(shown["files"][2]["text"] == "Keep threads small.");
  CHECK(WriteInstructionFile(false, false, cwd,
                             std::string(kProjectDocBytes + 1, 'x')) != "");
  // Two editors from the same text: the second save is refused, not lost.
  CHECK(WriteInstructionFile(false, true, cwd, "Mine.", "Run ctest.").empty());
  CHECK(WriteInstructionFile(false, true, cwd, "Theirs.", "Run ctest.")
            .find("changed since") != std::string::npos);
  CHECK(ReadInstructionFile(InstructionPath(false, true, cwd)) == "Mine.");
  // The editor edits the file the loader reads at that level, and never
  // writes through a link.
  { std::ofstream(cwd / "CLAUDE.md") << "Legacy."; }
  fs::remove(cwd / "AGENTS.md");
  CHECK(InstructionPath(false, true, cwd).filename() == "CLAUDE.md");
  CHECK(InstructionFiles(cwd)["files"][1]["text"] == "Legacy.");
  fs::remove(cwd / "CLAUDE.md");
  fs::create_symlink(cwd / "elsewhere", cwd / "AGENTS.md");
  CHECK(WriteInstructionFile(false, true, cwd, "x").find("symbolic link") !=
        std::string::npos);
  CHECK(!fs::exists(cwd / "elsewhere"));
}

void TestPromptRequestParity() {
  TestWorkspace workspace("prompt-request");
  Harness h;
  auto proposed = h.Set("replace", "Only this behavior.\nPreserve whitespace.");
  CHECK(proposed["effective"].get<std::string>().find("Read only what the task needs") ==
        std::string::npos);
  h.agent.PreviewContext();
  CHECK(h.agent.ModelRequest()["messages"][0]["content"] ==
        proposed["effective"]);
  const auto stable = h.agent.ModelRequest();
  h.agent.PreviewContext();
  CHECK(h.agent.ModelRequest() == stable);
  const auto path = (workspace.workspace / "session.json").string();
  std::string error;
  CHECK(h.agent.Save(path, error));
  h.state.mode = "overlay";
  h.state.instructions.clear();
  CHECK(h.agent.Load(path, workspace.workspace.string(), error));
  CHECK(h.state.mode == "replace");
  CHECK(h.agent.PromptPreview()["effective"] == proposed["effective"]);
}
}  // namespace uagent
