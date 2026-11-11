// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "include/core/fs.h"
#include "include/tools/registry.h"
#include "tests/unit/test_support.h"

namespace uagent {

// The agent's own scripts: written without asking, kept out of git, and
// shown to the person who approves the command that runs one.
TEST_CASE(TestScratchScripts) {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() /
                        ("uagent-scratch-test-" + std::to_string(getpid()));
  fs::create_directories(root);
  ActivitySupervisor supervisor;
  std::vector<Tool> tools = BuiltinTools(supervisor, root);
  CHECK(FindTool(tools, "scratch") == nullptr);
  const Tool* write = FindTool(tools, "write_file");
  const Tool* run = FindTool(tools, "run");
  CHECK(write && run);
  if (!write || !run) return;

  const std::string script = (root / ".uagent/scratch/sum.py").string();
  const json wrote = {{"path", script}, {"content", "print(1 + 2)\n"}};
  CHECK(!ToolMutates(*write, wrote));
  CHECK(ToolMutates(*write, {{"path", (root / "sum.py").string()}}));
  CHECK(write->run(wrote, {}).Ok());
  CHECK(ReadFile((root / ".uagent/scratch/.gitignore").string(), 16) == "*\n");
  // Its own script is rewritten without saying so; a file of the folder's
  // is not.
  CHECK(write->run(wrote, {}).Ok());
  const json kept = {{"path", (root / "kept.txt").string()}, {"content", "a"}};
  CHECK(write->run(kept, {}).Ok());
  CHECK(!write->run(kept, {}).Ok());

  const auto preview = [&](const std::string& command) {
    return run->approval_preview({{"command", command}});
  };
  CHECK(preview("uv run --script .uagent/scratch/sum.py 4") ==
        "uv run --script .uagent/scratch/sum.py 4\n\nsum.py:\nprint(1 + 2)\n");
  CHECK(preview("sh '" + script + "'").ends_with("sum.py:\nprint(1 + 2)\n"));
  // Nothing to show for a plain command, a script that is not there, one
  // outside the folder, or two at once.
  for (const char* command :
       {"ls -la", "sh .uagent/scratch/missing.sh",
        "sh ../.uagent/scratch/sum.py",
        "cat .uagent/scratch/sum.py .uagent/scratch/sum.py"}) {
    CHECK(preview(command) == command);
  }

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
