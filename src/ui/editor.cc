// Copyright 2026 Timon Gentzsch
#include "include/ui/editor.h"

#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "include/core/config.h"
#include "include/core/fs.h"
#include "include/core/platform.h"
#include "include/core/signals.h"

namespace uagent {
bool EditExternalText(std::string& text, int terminal_fd, size_t limit) {
  const char* editor = getenv("VISUAL");
  if (!editor || !*editor) editor = getenv("EDITOR");
  if (!editor || !*editor || !isatty(terminal_fd)) return false;
  ScopedTempFile draft(UagentDir("drafts") + "/edit-XXXXXX");
  if (!draft) return false;
  draft.Close();
  std::string error;
  bool ok = WritePrivateFile(draft.Path(), text, error);
  if (ok) {
    pid_t pid = 0;
    const int spawned = SpawnProcess(
        {.argv = {"sh", "-c",
                  std::string(editor) + " " + ShellQuote(draft.Path())},
         .fds = {{STDIN_FILENO, terminal_fd},
                 {STDOUT_FILENO, terminal_fd},
                 {STDERR_FILENO, terminal_fd}}},
        pid);
    int status = 0;
    const pid_t waited = spawned == 0 ? WaitPid(pid, &status) : -1;
    std::string edited;
    ok = waited == pid && ExitedOk(status) &&
         ReadRegularFile(draft.Path(), limit, edited, error);
    if (ok) text = std::move(edited);
  }
  return ok;
}
}  // namespace uagent
