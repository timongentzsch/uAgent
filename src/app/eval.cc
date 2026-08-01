// Copyright 2026 Timon Gentzsch

#include "include/app/eval.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "include/core/env.h"

namespace uagent {
namespace {

std::filesystem::path ExecutablePath(const char* executable) {
  std::error_code error;
  std::filesystem::path path = std::filesystem::canonical(executable, error);
  if (!error) return path;
  std::string search = EnvStr("PATH");
  for (size_t begin = 0; begin <= search.size();) {
    size_t end = search.find(':', begin);
    if (end == std::string::npos) end = search.size();
    path = std::filesystem::canonical(
        std::filesystem::path(search.substr(begin, end - begin)) / executable,
        error);
    if (!error) return path;
    begin = end + 1;
  }
  return executable;
}

std::string EvalScript(const std::filesystem::path& binary) {
  std::string configured = EnvStr("UAGENT_EVAL_SCRIPT");
  if (!configured.empty()) return configured;
  if (binary.is_absolute()) {
    std::filesystem::path installed =
        binary.parent_path().parent_path() / "share/uagent/eval.py";
    if (std::filesystem::is_regular_file(installed)) return installed.string();
    std::filesystem::path source =
        binary.parent_path().parent_path().parent_path() /
        "tests/agent_workflow_live.py";
    if (std::filesystem::is_regular_file(source)) return source.string();
  }
  return "tests/agent_workflow_live.py";
}

}  // namespace

int RunEval(int argc, char** argv) {
  std::filesystem::path binary = ExecutablePath(argv[0]);
  std::string script = EvalScript(binary);
  if (!std::filesystem::is_regular_file(script)) {
    fprintf(stderr, "uagent eval: evaluator not found: %s\n", script.c_str());
    return 2;
  }
  std::vector<std::string> values = {"uv",           "run",   "--no-project",
                                     script,         "--run", "--binary",
                                     binary.string()};
  for (int index = 2; index < argc; ++index) values.emplace_back(argv[index]);
  std::vector<char*> arguments;
  arguments.reserve(values.size() + 1);
  for (std::string& value : values) arguments.push_back(value.data());
  arguments.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    perror("uagent eval");
    return 2;
  }
  if (pid == 0) {
    execvp(arguments[0], arguments.data());
    perror("uagent eval requires uv");
    _exit(127);
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) return 2;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

}  // namespace uagent
