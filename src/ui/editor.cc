// Copyright 2026 Timon Gentzsch
#include "include/ui/editor.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "include/core/config.h"
#include "include/core/fs.h"
#include "include/core/signals.h"

namespace uagent {
bool EditExternalText(std::string& text, int terminal_fd, size_t limit) {
  const char* editor = getenv("VISUAL");
  if (!editor || !*editor) editor = getenv("EDITOR");
  if (!editor || !*editor || !isatty(terminal_fd)) return false;
  std::string path = UagentDir("drafts") + "/edit-XXXXXX";
  int fd = mkstemp(path.data());
  if (fd < 0) return false;
  close(fd);
  std::string error;
  bool ok = AtomicWriteFile(path, text, kPrivateFileMode, false, error);
  if (ok) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int target : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
      posix_spawn_file_actions_adddup2(&actions, terminal_fd, target);
    }
    const auto command = std::string(editor) + " " + ShellQuote(path);
    const char* argv[] = {"sh", "-c", command.c_str(), nullptr};
    pid_t pid = 0;
    const int spawned =
        posix_spawnp(&pid, "sh", &actions, nullptr,
                     const_cast<char* const*>(argv), ProcessEnvironment());
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    pid_t waited = -1;
    if (spawned == 0) {
      do {
        waited = waitpid(pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
    }
    std::string edited;
    ok = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         ReadRegularFile(path, limit, edited, error);
    if (ok) text = std::move(edited);
  }
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return ok;
}
}  // namespace uagent
