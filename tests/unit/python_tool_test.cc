// Copyright 2026 Timon Gentzsch

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "include/tools/files.h"
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
  std::vector<Tool> python_tools = BuiltinTools(supervisor, root, false);
  const Tool* run = FindTool(python_tools, "run");
  CHECK(run && ToolDescription(*run).find("omit cd") != std::string::npos);
  setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

  ToolResult result = ToolRunScratch(supervisor, root, "math.py",
                                     "print(6 * 7)", json::array({"numpy>=2"}));
  CHECK(result.output ==
        "[script: .uagent/scratch/math.py · wrote · executed]\n42\n");
  fs::path script = root / ".uagent/scratch/math.py";
  CHECK(fs::is_regular_file(script));
  CHECK(fs::is_regular_file(root / ".uagent/scratch/.gitignore"));
  result = ToolRunScratch(supervisor, root, ".uagent/scratch/prefixed.py",
                          "print('normalized')", json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/prefixed.py · wrote · executed]\n"
        "normalized\n");
  CHECK(fs::is_regular_file(root / ".uagent/scratch/prefixed.py"));
  std::ifstream script_input(script);
  std::string script_source{std::istreambuf_iterator<char>(script_input),
                            std::istreambuf_iterator<char>()};
  CHECK(script_source.find("# /// script") == 0);
  CHECK(script_source.find("\"numpy>=2\"") != std::string::npos);

  result = ToolRunScratch(supervisor, root, "math.py", "print(6 * 7)",
                          json::array({"numpy>=2"}));
  CHECK(result.error == ToolErrorCode::kInvalidArguments);
  CHECK(result.output.find("code is identical") != std::string::npos);
  CHECK(result.output.find("code=null") != std::string::npos);

  result = ToolRunScratch(supervisor, root, "math.py", "print(7 * 7)",
                          json::array({"numpy>=2"}));
  CHECK(result.output ==
        "[script: .uagent/scratch/math.py · overwrote · executed]\n49\n");

  CHECK(ToolEditFile(script.string(), {{"7 * 7", "8 * 8", false}}).Ok());
  result = ToolRunScratch(supervisor, root, "math.py", nullptr, nullptr);
  CHECK(result.output == "[script: .uagent/scratch/math.py · executed]\n64\n");

  // argv lets one saved script answer a family of questions, so a changed
  // parameter is a rerun rather than a rewritten body.
  result = ToolRunScratch(supervisor, root, "argv.py",
                          "import sys; print('|'.join(sys.argv[1:]))",
                          json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/argv.py · wrote · executed]\n\n");
  result = ToolRunScratch(supervisor, root, "argv.py", nullptr, nullptr,
                          json::array({"a b", "--limit=5"}));
  CHECK(result.output ==
        "[script: .uagent/scratch/argv.py 'a b' --limit=5 · executed]\n"
        "a b|--limit=5\n");
  result = ToolRunScratch(supervisor, root, "argv.sh", "echo \"$2/$1\"\n",
                          json::array(), json::array({"one", "two"}));
  CHECK(result.output ==
        "[script: .uagent/scratch/argv.sh one two · wrote · executed]\n"
        "two/one\n");
  result = ToolRunScratch(supervisor, root, "argv.sh", nullptr, nullptr,
                          json::array({7}));
  CHECK(result.error == ToolErrorCode::kInvalidArguments);
  CHECK(result.output.find("args must be strings") != std::string::npos);

  // The body is worth the scrollback once per path; a rewrite reports counts.
  result = ToolRunScratch(supervisor, root, "receipt.py", "print(1)",
                          json::array());
  CHECK(result.display.starts_with("Created "));
  CHECK(result.display.find("+print(1)") != std::string::npos);
  result = ToolRunScratch(supervisor, root, "receipt.py", "print(2)",
                          json::array());
  CHECK(result.display.starts_with("Replaced "));
  CHECK(result.display.find('\n') == result.display.size() - 1);
  CHECK(result.display.find("print(2)") == std::string::npos);

  fs::path marker = root / "injected";
  result = ToolRunScratch(supervisor, root, "safe.py", "print('safe')",
                          json::array({"x; touch " + marker.string()}));
  CHECK(result.output ==
        "[script: .uagent/scratch/safe.py · wrote · executed]\nsafe\n");
  CHECK(!fs::exists(marker));

  result = ToolRunScratch(supervisor, root, "slow.py",
                          "import time; time.sleep(.2); print('slow-ok')",
                          json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/slow.py · wrote · executed]\nslow-ok\n");
  CHECK(supervisor.PendingCount() == 0);

  result =
      ToolRunScratch(supervisor, root, "missing.py",
                     "import definitely_missing_uagent_package", json::array());
  CHECK(result.error == ToolErrorCode::kProcessFailed);
  CHECK(result.output.find("error: Python execution failed.") !=
        std::string::npos);
  CHECK(result.output.find("PEP 723 header") != std::string::npos);

  // A shell script is the same save-once-rerun mechanism without an
  // interpreter: half of every run command this agent sends is a byte-exact
  // repeat, and only .py could be saved and rerun by path.
  result = ToolRunScratch(supervisor, root, "pipe.sh",
                          "echo one two | sed 's/ /-/'\n", json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/pipe.sh · wrote · executed]\none-two\n");
  result = ToolRunScratch(supervisor, root, "pipe.sh", nullptr, nullptr);
  CHECK(result.output ==
        "[script: .uagent/scratch/pipe.sh · executed]\none-two\n");

  result = ToolRunScratch(supervisor, root, "deps.sh", "true\n",
                          json::array({"numpy"}));
  CHECK(result.error == ToolErrorCode::kInvalidArguments);
  CHECK(result.output.find("takes no packages") != std::string::npos);

  // The privileged-command rule `run` applies to a command, applied to every
  // line -- a first-word check against a body would only see line one.
  result = ToolRunScratch(supervisor, root, "priv.sh",
                          "echo fine\nsudo rm -rf /\n", json::array());
  CHECK(result.error == ToolErrorCode::kPermissionDenied);
  CHECK(result.output.find("line 2") != std::string::npos);
  CHECK(result.output.find("Do not use sudo") != std::string::npos);
  CHECK(!fs::exists(root / ".uagent/scratch/priv.sh.ran"));

  // Gated at execution, not at write: the saved file is editable between a
  // write and a `code: null` rerun, so a check done only on the way in would
  // be theatre.
  result = ToolRunScratch(supervisor, root, "later.sh", "echo before\n",
                          json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/later.sh · wrote · executed]\nbefore\n");
  CHECK(ToolEditFile((root / ".uagent/scratch/later.sh").string(),
                     {{"echo before", "sudo rm -rf /", false}})
            .Ok());
  result = ToolRunScratch(supervisor, root, "later.sh", nullptr, nullptr);
  CHECK(result.error == ToolErrorCode::kPermissionDenied);
  CHECK(result.output.find("Do not use sudo") != std::string::npos);

  result =
      ToolRunScratch(supervisor, root, "other.rb", "puts 1", json::array());
  CHECK(result.error == ToolErrorCode::kInvalidArguments);
  CHECK(result.output.find(".py or .sh") != std::string::npos);

  result = ToolRunScratch(supervisor, root, "../escape.py", "print('x')",
                          json::array());
  CHECK(result.error == ToolErrorCode::kPermissionDenied);
  result = ToolRunScratch(supervisor, root, "math.py", nullptr, json::array());
  CHECK(result.error == ToolErrorCode::kInvalidArguments);

  setenv("PATH", root.c_str(), 1);  // no uv
  result = ToolRunScratch(supervisor, root, "dependency.py", "print('x')",
                          json::array({"numpy"}));
  CHECK(result.error == ToolErrorCode::kUnavailable);
  CHECK(result.output.find("declares third-party dependencies") !=
        std::string::npos);

  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }
  auto tools = BuiltinTools(supervisor, root, false);
  CHECK(FindTool(tools, "scratch") != nullptr);

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
