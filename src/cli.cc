// Copyright 2026 Timon Gentzsch

#include "include/cli.h"

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#if defined(HAVE_EDITLINE)
#include <editline/readline.h>
#endif

#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

namespace {
InteractiveReadHandler& ReadHandler() {
  static InteractiveReadHandler handler;
  return handler;
}
}  // namespace

constexpr SlashCommandSpec kSlashCommands[] = {
    {SlashCommandId::kAttach, "/attach", "PATH|clear",
     "attach a file to the next turn", CommandCompletion::kFilenames},
    {SlashCommandId::kCompact, "/compact", "", "summarize active context",
     CommandCompletion::kNone},
    {SlashCommandId::kContext, "/context", "", "show current model request",
     CommandCompletion::kNone},
    {SlashCommandId::kCost, "/cost", "", "show spend by model route",
     CommandCompletion::kNone},
    {SlashCommandId::kEffort, "/effort", "LEVEL", "set reasoning effort",
     CommandCompletion::kEfforts},
    {SlashCommandId::kHelp, "/help", "", "show this help",
     CommandCompletion::kNone},
    {SlashCommandId::kHandoff, "/handoff", "PROVIDER/MODEL",
     "checkpoint and switch route", CommandCompletion::kModels},
    {SlashCommandId::kMemory, "/memory", "", "show memory state and keys",
     CommandCompletion::kNone},
    {SlashCommandId::kModel, "/model", "NAME", "switch model or route",
     CommandCompletion::kModels},
    {SlashCommandId::kModels, "/models", "[QUERY]",
     "search and select across providers", CommandCompletion::kNone},
    {SlashCommandId::kOnline, "/online", "", "toggle OpenRouter web search",
     CommandCompletion::kNone},
    {SlashCommandId::kQuit, "/quit", "", "exit µAgent",
     CommandCompletion::kNone},
    {SlashCommandId::kReset, "/reset", "", "start a fresh session",
     CommandCompletion::kNone},
    {SlashCommandId::kSessions, "/sessions", "", "resume a saved session",
     CommandCompletion::kNone},
    {SlashCommandId::kTrace, "/trace", "", "show latest tool and search trace",
     CommandCompletion::kNone},
    {SlashCommandId::kVerbose, "/verbose", "", "toggle full tool traces",
     CommandCompletion::kNone},
    {SlashCommandId::kYolo, "/yolo", "", "toggle automatic approval",
     CommandCompletion::kNone},
    {SlashCommandId::kQuit, "/exit", "", "", CommandCompletion::kNone},
    {SlashCommandId::kQuit, "/q", "", "", CommandCompletion::kNone},
    {SlashCommandId::kReset, "/clear", "", "", CommandCompletion::kNone},
    {SlashCommandId::kReset, "/new", "", "", CommandCompletion::kNone},
    {SlashCommandId::kContext, "/ctx", "", "", CommandCompletion::kNone},
};

const SlashCommandSpec* SlashCommand(const std::string& name) {
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (name == command.name) return &command;
  }
  return nullptr;
}

ParsedSlashCommand ParseSlashCommand(const std::string& input) {
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (input == command.name) return {&command, ""};
    std::string prefix = std::string(command.name) + " ";
    if (*command.argument && input.starts_with(prefix)) {
      return {&command, Trim(input.substr(prefix.size()))};
    }
  }
  return {};
}

void PrintCommandHelp() {
  size_t width = 0;
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (!*command.description) continue;
    width = std::max(
        width, strlen(command.name) +
                   (*command.argument ? strlen(command.argument) + 1 : 0));
  }
  printf("%scommands%s\n", BOLD(), RST());
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (!*command.description) continue;
    std::string usage = command.name;
    if (*command.argument) usage += " " + std::string(command.argument);
    printf("  %s%-*s%s  %s%s%s\n", BOLD(), static_cast<int>(width),
           usage.c_str(), RST(), DIM(), command.description, RST());
  }
}

#if defined(HAVE_EDITLINE)
namespace {

constexpr int kInputHistoryEntries = 200;
constexpr size_t kInputHistoryEntryBytes = 16 * 1024;

struct ReadlineState {
  std::queue<unsigned char> pending;  // bytes read ahead, replayed first
  std::string initial;
  size_t initial_pos = 0;
  bool cancel_on_escape = false;
  bool cancelled = false;
  bool resize_handler_installed = false;
  std::vector<std::string> commands;
  std::vector<std::string> models;
  std::vector<std::string> efforts;
  const std::vector<std::string>* candidates = nullptr;
  int rows = 0;
  int columns = 0;
};

ReadlineState& Readline() {
  static ReadlineState state;
  return state;
}

void RememberInput(const std::string& input) {
  if (!Trim(input).empty() && input.size() <= kInputHistoryEntryBytes) {
    add_history(input.c_str());
  }
}

}  // namespace

void RefreshReadlineSize(FILE* file) {
  int fd = file ? fileno(file) : STDIN_FILENO;
  struct winsize size{};
  if (ioctl(fd, TIOCGWINSZ, &size) != 0 || size.ws_row == 0 ||
      size.ws_col == 0) {
    return;
  }
  int rows = size.ws_row;
  int columns = size.ws_col;
  if (rows == Readline().rows && columns == Readline().columns) return;
  Readline().rows = rows;
  Readline().columns = columns;
  rl_set_screen_size(rows, columns);
}

char* CompletionCandidate(const char* text, int state) {
  static size_t index;
  if (!state) index = 0;
  while (Readline().candidates && index < Readline().candidates->size()) {
    const std::string& candidate = (*Readline().candidates)[index++];
    if (candidate.starts_with(text)) return strdup(candidate.c_str());
  }
  return nullptr;
}

char** UagentCompletion(const char* text, int start, int) {
  rl_attempted_completion_over = 1;
  Readline().candidates = nullptr;
  std::string before = rl_line_buffer ? std::string(rl_line_buffer, start) : "";
  if (start == 0 && *text == '/') {
    Readline().candidates = &Readline().commands;
  } else if (const SlashCommandSpec* command = SlashCommand(Trim(before))) {
    if (command->completion == CommandCompletion::kModels) {
      Readline().candidates = &Readline().models;
    } else if (command->completion == CommandCompletion::kEfforts) {
      Readline().candidates = &Readline().efforts;
    } else if (command->completion == CommandCompletion::kFilenames) {
      rl_attempted_completion_over = 0;
      return nullptr;
    }
  } else if (before.empty() || before[0] != '/') {
    rl_attempted_completion_over = 0;
    return nullptr;
  }
  // libedit uses `char*` here on Linux and `const char*` on macOS.
  return Readline().candidates
             ? completion_matches(const_cast<char*>(text), CompletionCandidate)
             : nullptr;
}

int ReadlineGetc(FILE* file) {
  // libedit replaces SIGWINCH with a handler that resizes heap-backed state
  // inside signal context. Reclaim the signal once its read loop has started;
  // our handler only sets a flag and the resize stays in ordinary code below.
  if (!Readline().resize_handler_installed) {
    InstallSigwinchHandler();
    Readline().resize_handler_installed = true;
  }
  if (Readline().initial_pos < Readline().initial.size()) {
    return static_cast<unsigned char>(
        Readline().initial[Readline().initial_pos++]);
  }
  if (!Readline().pending.empty()) {
    int ch = Readline().pending.front();
    Readline().pending.pop();
    return ch;
  }
  int fd = file ? fileno(file) : 0;
  unsigned char ch;
  for (;;) {
    if (g_terminal_resized) {
      g_terminal_resized = 0;
      // A forced libedit redisplay prints another prompt without first
      // clearing the old one. Mobile SSH clients emit resize bursts while
      // reconnecting, producing "> > >". Updating the dimensions is enough;
      // libedit redraws on the next input event.
      RefreshReadlineSize(file);
    }
    ssize_t size = read(fd, &ch, 1);
    if (size < 0 && errno == EINTR) continue;
    return size > 0 ? ch : EOF;
  }
}

int EscGetc(FILE* file) {
  int ch = ReadlineGetc(file);
  if (ch == 0x1b) {
    int fd = file ? fileno(file) : 0;
    pollfd event{fd, POLLIN, 0};
    if (poll(&event, 1, 50) == 0) {
      if (Readline().cancel_on_escape) {
        Readline().cancelled = true;
        return '\n';
      }
      Readline().pending.push(0x0b);
      return 0x01;
    }
    static const std::string kPasteStart = "[200~";
    std::string sequence;
    for (char expected : kPasteStart) {
      int next = ReadlineGetc(file);
      if (next == EOF) break;
      sequence += static_cast<char>(next);
      if (next != expected) break;
    }
    if (sequence == kPasteStart) {
      static const std::string kPasteEnd = "\x1b[201~";
      std::string paste;
      for (int next; (next = ReadlineGetc(file)) != EOF;) {
        paste += static_cast<char>(next);
        if (paste.size() >= kPasteEnd.size() &&
            paste.compare(paste.size() - kPasteEnd.size(), kPasteEnd.size(),
                          kPasteEnd) == 0) {
          paste.resize(paste.size() - kPasteEnd.size());
          break;
        }
      }
      ReplaceAll(paste, "\r\n", "\n");
      ReplaceAll(paste, "\r", "\n");
      std::erase(paste, '\0');
      rl_insert_text(paste.c_str());
      return 0x12;  // Ctrl-R / ed-redisplay; Enter still submits separately.
    }
    for (unsigned char byte : sequence) Readline().pending.push(byte);
  }
  return ch;
}

void RegisterCompletion(CommandCompletion source, const std::string& value) {
  std::vector<std::string>* values = nullptr;
  if (source == CommandCompletion::kModels) values = &Readline().models;
  if (source == CommandCompletion::kEfforts) values = &Readline().efforts;
  if (!values || value.empty() ||
      std::find(values->begin(), values->end(), value) != values->end()) {
    return;
  }
  values->push_back(value);
}

void ConfigureCompletion(const std::vector<std::string>& models,
                         const std::vector<std::string>& efforts) {
  Readline().commands.clear();
  Readline().models = models;
  Readline().efforts = efforts;
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (*command.description) Readline().commands.push_back(command.name);
  }
  std::sort(Readline().models.begin(), Readline().models.end());
  std::sort(Readline().efforts.begin(), Readline().efforts.end());
  Readline().models.erase(
      std::unique(Readline().models.begin(), Readline().models.end()),
      Readline().models.end());
  Readline().efforts.erase(
      std::unique(Readline().efforts.begin(), Readline().efforts.end()),
      Readline().efforts.end());
  rl_attempted_completion_function = UagentCompletion;
}

std::string RlHideEscapes(const std::string& prompt) {
  std::string out;
  for (size_t i = 0; i < prompt.size();) {
    if (prompt[i] == '\033') {
      size_t end = i + 1;
      if (end < prompt.size() && prompt[end] == '[') {
        for (++end; end < prompt.size() &&
                    !isalpha(static_cast<unsigned char>(prompt[end]));
             ++end) {
        }
        if (end < prompt.size()) ++end;
      }
      out += '\001';
      out.append(prompt, i, end - i);
      out += '\002';
      i = end;
    } else {
      out += prompt[i++];
    }
  }
  return out;
}
#endif

void ConfigureLineEditor() {
#if defined(HAVE_EDITLINE)
  rl_getc_function = EscGetc;
  stifle_history(kInputHistoryEntries);
#endif
}

void SetInteractiveReadHandler(InteractiveReadHandler handler) {
  ReadHandler() = std::move(handler);
}

std::string InputPrompt(const char* label) {
  return std::string(BOLD()) +
         (label && *label ? std::string(label) + "> " : "> ");
}

#if defined(HAVE_EDITLINE)
namespace {

std::string ReadEditableLine(const std::string& prompt, bool* eof,
                             bool keep_history, const std::string& initial,
                             bool cancel_on_escape, bool* cancelled) {
  Readline().cancel_on_escape = cancel_on_escape;
  Readline().cancelled = false;
  std::string hidden_prompt = RlHideEscapes(prompt);
  RefreshReadlineSize(stdin);
  Readline().initial = initial;
  Readline().initial_pos = 0;
  Readline().resize_handler_installed = false;
  BracketedPaste(true);
  char* line = readline(hidden_prompt.c_str());
  BracketedPaste(false);
  Readline().initial.clear();
  Readline().initial_pos = 0;
  fputs(RST(), stdout);
  TerminalClearToEnd();
  if (!line) {
    Readline().cancel_on_escape = false;
    if (cancelled) *cancelled = Readline().cancelled;
    Readline().cancelled = false;
    *eof = true;
    return "";
  }
  std::string input = line;
  free(line);
  Readline().cancel_on_escape = false;
  if (cancelled) *cancelled = Readline().cancelled;
  Readline().cancelled = false;
  if (keep_history) RememberInput(input);
  return input;
}

}  // namespace
#endif

std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history, const std::string& initial) {
  (void)keep_history;  // used only when libedit is compiled in
  *eof = false;
  if (ReadHandler()) {
    return ReadHandler()(prompt, eof, keep_history, initial);
  }
#if defined(HAVE_EDITLINE)
  if (g_tty) {
    return ReadEditableLine(prompt, eof, keep_history, initial,
                            /*cancel_on_escape=*/false, nullptr);
  }
#endif
  fputs(prompt.c_str(), stdout);
  fputs(initial.c_str(), stdout);
  fflush(stdout);
  std::string input;
  if (!std::getline(std::cin, input)) {
    fputs(RST(), stdout);
    *eof = true;
    return "";
  }
  fputs(RST(), stdout);
  return initial + input;
}

std::string ReadChoiceLine(const std::string& prompt, bool& cancelled,
                           bool& eof) {
  if (ReadHandler()) {
    std::string input = Trim(ReadInputLine(prompt, &eof, false));
    cancelled = eof;
    return cancelled ? "" : input;
  }
#if defined(HAVE_EDITLINE)
  if (g_tty) {
    std::string input =
        Trim(ReadEditableLine(prompt, &eof, /*keep_history=*/false, "",
                              /*cancel_on_escape=*/true, &cancelled));
    return cancelled ? "" : input;
  }
#else
  (void)cancelled;
#endif
  std::string input = Trim(ReadInputLine(prompt, &eof, false));
  cancelled = input.find('\x1b') != std::string::npos;
  return cancelled ? "" : input;
}

}  // namespace uagent
