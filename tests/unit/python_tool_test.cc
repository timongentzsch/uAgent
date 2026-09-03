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
            "  if [ \"$1\" = --script ]; then shift; exec python3 \"$1\"; fi\n"
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

  ToolResult result = ToolRunScratch(supervisor, root, "math.py", "print(6 * 7)",
                                    json::array({"numpy>=2"}));
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
