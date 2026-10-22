// Copyright 2026 Timon Gentzsch

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/tools/files.h"
#include "include/tools/registry.h"
#include "include/tools/session.h"
#include "include/tools/shell.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestPythonTool() {
  namespace fs = std::filesystem;
  CHECK(!PythonScriptHasDependencies(
      "# /// script\n# dependencies = [\n# ]\n# ///\n"));
  CHECK(PythonScriptHasDependencies(
      "# /// script\n# dependencies = [\n#   \"numpy\",\n# ]\n# ///\n"));
  CHECK(
      PythonScriptHasDependencies("# /// script\n# dependencies = [\n# ///\n"));
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-python-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::path bin = root / "bin";
  fs::create_directories(bin);
  fs::path uv = bin / "uv";
  CHECK(ToolWriteFile(
            uv.string(),
            "#!/bin/sh\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  if [ \"$1\" = --script ]; then shift; exec python3 \"$@\"; fi\n"
            "  shift\n"
            "done\n"
            "exit 2\n")
            .output.starts_with("wrote "));
  CHECK(chmod(uv.c_str(), 0700) == 0);

  const char* prior_path_value = getenv("PATH");
  std::string prior_path = prior_path_value ? prior_path_value : "";
  ProcessSupervisor supervisor;
  std::vector<Tool> python_tools = BuiltinTools(supervisor, root);
  const Tool* run = FindTool(python_tools, "run");
  CHECK(run && ToolDescription(*run).find("omit cd") != std::string::npos);
  setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

  // Scripts are written with the file tools and only run here.
  fs::path scratch = root / ".uagent/scratch";
  auto write = [&](const std::string& name, const std::string& source) {
    CHECK(ToolWriteFile((scratch / name).string(), source).Ok());
  };
  // Writing into scratch is the agent's own working state, not a change a
  // person approves; the same write anywhere else still is.
  const Tool* write_file = FindTool(python_tools, "write_file");
  CHECK(write_file &&
        !ToolMutates(*write_file, {{"path", (scratch / "math.py").string()}}) &&
        ToolMutates(*write_file, {{"path", (root / "math.py").string()}}));

  ToolResult result = ToolRunScratch(supervisor, root, "math.py");
  CHECK(result.error == ToolErrorCode::kNotFound);
  CHECK(result.output.find("this call supplied no code") != std::string::npos);
  CHECK(result.output.find("Pass code with path to scratch") !=
        std::string::npos);

  write("math.py",
        "# /// script\n# dependencies = [\"numpy>=2\"]\n# ///\nprint(6 * 7)\n");
  result = ToolRunScratch(supervisor, root, "math.py");
  CHECK(result.output == "42\n");
  CHECK(fs::is_regular_file(scratch / ".gitignore"));
  result = ToolRunScratch(supervisor, root, ".uagent/scratch/math.py");
  CHECK(result.output == "42\n");
  // A script runs in the workspace, so a file it names without a folder
  // lands in the root: the result says what the run left there, and nothing
  // of what was there before or of a run that left nothing.
  const fs::path prior_directory = fs::current_path();
  fs::current_path(root);
  CHECK(ToolWriteFile((root / "kept.txt").string(), "old").Ok());
  write("save.sh", "echo x > chart.png; mkdir -p out; echo x > out/data.csv\n");
  result = ToolRunScratch(supervisor, root, "save.sh");
  CHECK(result.output == "[changed in the workspace: chart.png, out/]");
  write("quiet.sh", "echo done\n");
  result = ToolRunScratch(supervisor, root, "quiet.sh");
  CHECK(result.output == "done\n");
  fs::current_path(prior_directory);

  CHECK(
      ToolEditFile((scratch / "math.py").string(), {{"6 * 7", "8 * 8", false}})
          .Ok());
  CHECK(ToolRunScratch(supervisor, root, "math.py").output == "64\n");

  // argv lets one saved script answer a family of questions, so a changed
  // parameter is a rerun rather than a rewritten body.
  write("argv.py", "import sys; print('|'.join(sys.argv[1:]))\n");
  result = ToolRunScratch(supervisor, root, "argv.py",
                          json::array({"a b", "--limit=5"}));
  CHECK(result.output == "a b|--limit=5\n");
  write("argv.sh", "echo \"$2/$1\"\n");
  CHECK(ToolRunScratch(supervisor, root, "argv.sh", json::array({"one", "two"}))
            .output == "two/one\n");
  result = ToolRunScratch(supervisor, root, "argv.sh", json::array({7}));
  CHECK(result.error == ToolErrorCode::kInvalidArguments);
  CHECK(result.output.find("args must be strings") != std::string::npos);

  // Arguments are data, never shell syntax.
  fs::path marker = root / "injected";
  write("safe.py", "print('safe')\n");
  result = ToolRunScratch(supervisor, root, "safe.py",
                          json::array({"x; touch " + marker.string()}));
  CHECK(result.output == "safe\n");
  CHECK(!fs::exists(marker));
  CHECK(result.display.empty());  // a run is not a change receipt

  write("slow.py", "import time; time.sleep(.2); print('slow-ok')\n");
  CHECK(ToolRunScratch(supervisor, root, "slow.py").output == "slow-ok\n");
  CHECK(supervisor.PendingCount() == 0);

  write("missing.py", "import definitely_missing_uagent_package\n");
  result = ToolRunScratch(supervisor, root, "missing.py");
  CHECK(result.error == ToolErrorCode::kProcessFailed);
  CHECK(result.output.find("error: Python execution failed.") !=
        std::string::npos);
  CHECK(result.output.find("PEP 723 header") != std::string::npos);

  // Use a harmless executable named sudo: scripts follow the shared spawn
  // policy, without a separate keyword ban.
  CHECK(
      ToolWriteFile((bin / "sudo").string(), "#!/bin/sh\nexec \"$@\"\n").Ok());
  CHECK(chmod((bin / "sudo").c_str(), 0700) == 0);
  write("priv.sh", "sudo printf before\n");
  result = ToolRunScratch(supervisor, root, "priv.sh");
  CHECK(result.Ok());
  CHECK(result.output.ends_with("before"));

  // The approval a person sees is the script itself.
  const Tool* scratch_tool = FindTool(python_tools, "scratch");
  CHECK(scratch_tool && scratch_tool->approval_preview({{"path", "priv.sh"}}) ==
                            "sudo printf before\n");

  // With code the script is written and run in the one call, and the code
  // is what a person approves.
  const json inline_call = {{"path", "inline.sh"},
                            {"code", "printf inline-ran\n"}};
  CHECK(scratch_tool &&
        scratch_tool->approval_preview(inline_call) == "printf inline-ran\n");
  result = scratch_tool ? scratch_tool->run(inline_call, {}) : ToolResult{};
  CHECK(result.Ok());
  CHECK(result.output.ends_with("inline-ran"));
  CHECK(ToolRunScratch(supervisor, root, "inline.sh")
            .output.ends_with("inline-ran"));
  // What runs is what the approval showed: code that is given, even empty,
  // replaces the script, so an old one never runs behind an empty preview.
  const json emptied = {{"path", "inline.sh"}, {"code", ""}};
  CHECK(scratch_tool && scratch_tool->approval_preview(emptied).empty());
  result = scratch_tool ? scratch_tool->run(emptied, {}) : ToolResult{};
  CHECK(result.output.find("inline-ran") == std::string::npos);

  // Every parameter has one plain type: a union ("string or null") breaks
  // the tool-call conversion some providers do for a model.
  std::function<bool(const json&)> plain = [&](const json& schema) {
    if (schema.is_object() && schema.contains("type") &&
        schema["type"].is_array()) {
      return false;
    }
    return !schema.is_structured() || std::ranges::all_of(schema, plain);
  };
  for (const Tool& tool : python_tools) CHECK(plain(tool.parameters));
  CHECK(plain(SessionTool().parameters));

  CHECK(
      ToolRunScratch(supervisor, root, "other.rb").output.find(".js or .mjs") !=
      std::string::npos);
  CHECK(ToolRunScratch(supervisor, root, "../escape.py").error ==
        ToolErrorCode::kInvalidArguments);

  if (ExecutableOnPath("node")) {
    write("argv.mjs",
          "await Promise.resolve(); console.log(process.argv[2]);\n");
    CHECK(ToolRunScratch(supervisor, root, "argv.mjs", json::array({"a b; c"}))
              .output == "a b; c\n");
    write("fail.js", "process.exit(7);\n");
    CHECK(ToolRunScratch(supervisor, root, "fail.js").error ==
          ToolErrorCode::kProcessFailed);
  }
  CHECK(ToolRunScratch(supervisor, root, "../escape.mjs").error ==
        ToolErrorCode::kInvalidArguments);
  CHECK(ToolWriteFile((root / "outside.mjs").string(), "throw 1;").Ok());
  fs::create_symlink(root / "outside.mjs", scratch / "escape.mjs");
  CHECK(ToolRunScratch(supervisor, root, "escape.mjs").error ==
        ToolErrorCode::kInvalidArguments);

  setenv("PATH", root.c_str(), 1);  // no uv or node
  write("missing-node.mjs", "console.log('must not run');\n");
  CHECK(ToolRunScratch(supervisor, root, "missing-node.mjs").error ==
        ToolErrorCode::kUnavailable);
  write("dependency.py",
        "# /// script\n# dependencies = [\"numpy\"]\n# ///\nprint('x')\n");
  result = ToolRunScratch(supervisor, root, "dependency.py");
  CHECK(result.error == ToolErrorCode::kUnavailable);
  CHECK(result.output.find("declares third-party dependencies") !=
        std::string::npos);

  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
