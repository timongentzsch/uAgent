// Copyright 2026 Timon Gentzsch

#include "include/cli.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include "include/core/events.h"
#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

namespace {
InteractiveReadHandler& ReadHandler() {
  static InteractiveReadHandler handler;
  return handler;
}

// Startup prompts run before the persistent raw-mode composer is mounted. A
// prior interrupted terminal application can leave ICRNL disabled, in which
// case a real Enter arrives as an ordinary carriage return and getline waits
// forever while the terminal echoes "^M". Temporarily establish the minimum
// cooked-mode contract needed by the fallback line reader, then preserve the
// caller's original mode for the persistent composer or shell to own.
class ScopedCookedInput {
 public:
  ScopedCookedInput() {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_) != 0) return;
    termios cooked = saved_;
    cooked.c_lflag |= static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    cooked.c_iflag |= ICRNL;
    cooked.c_iflag &= ~static_cast<tcflag_t>(INLCR | IGNCR);
    active_ = tcsetattr(STDIN_FILENO, TCSANOW, &cooked) == 0;
  }

  ~ScopedCookedInput() {
    if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  }

 private:
  termios saved_{};
  bool active_ = false;
};
}  // namespace

constexpr SlashCommandSpec kSlashCommands[] = {
    {SlashCommandId::kAttach, "/attach", "PATH|clear",
     "attach a file to the next turn"},
    {SlashCommandId::kCompact, "/compact", "",
     "summarize conversation to prevent hitting the context limit"},
    {SlashCommandId::kContext, "/context", "", "show current model request"},
    {SlashCommandId::kDiff, "/diff", "",
     "show git diff (including untracked files)"},
    {SlashCommandId::kCost, "/cost", "", "show tokens and spend by route"},
    {SlashCommandId::kDebugConfig, "/debug-config", "[SETTING]",
     "show configuration layers, sources and restart-required fields"},
    {SlashCommandId::kEffort, "/effort", "LEVEL",
     "choose how much reasoning effort to use"},
    {SlashCommandId::kHelp, "/help", "", "show this help"},
    {SlashCommandId::kInit, "/init", "",
     "create an AGENTS.md file with instructions for \u00b5Agent"},
    {SlashCommandId::kMemory, "/memory", "", "show memory state and keys"},
    {SlashCommandId::kModel, "/model", "NAME", "choose what model to use"},
    {SlashCommandId::kModels, "/models", "[QUERY]",
     "search and select across providers"},
    {SlashCommandId::kProcesses, "/ps", "", "list background work"},
    {SlashCommandId::kQuit, "/quit", "", "exit µAgent"},
    {SlashCommandId::kReset, "/reset", "", "start a new chat"},
    {SlashCommandId::kReview, "/review", "[TARGET]",
     "review my current changes and find issues"},
    {SlashCommandId::kSessions, "/sessions", "", "resume a saved chat"},
    {SlashCommandId::kStatus, "/status", "",
     "show current session configuration and token usage"},
    {SlashCommandId::kTools, "/tools", "", "show tools available right now"},
    {SlashCommandId::kTrace, "/trace", "", "show latest tool and search trace"},
    {SlashCommandId::kVariant, "/variant", "MODE",
     "set OpenRouter provider routing"},
    {SlashCommandId::kVerbose, "/verbose", "",
     "toggle full reasoning and expanded tool output"},
    {SlashCommandId::kYolo, "/yolo", "", "toggle automatic approval"},
    {SlashCommandId::kQuit, "/exit", "", ""},
    {SlashCommandId::kQuit, "/q", "", ""},
    {SlashCommandId::kReset, "/clear", "", ""},
    {SlashCommandId::kReset, "/new", "", ""},
    {SlashCommandId::kContext, "/ctx", "", ""},
    {SlashCommandId::kSessions, "/resume", "", ""},
    {SlashCommandId::kYolo, "/permissions", "", ""},
};

std::span<const SlashCommandSpec> SlashCommandRegistry() {
  return kSlashCommands;
}

std::string SlashCommandPrompt(const ParsedSlashCommand& command) {
  if (!command.spec) return {};
  switch (command.spec->id) {
    case SlashCommandId::kInit:
      return "Write AGENTS.md in the repository root with instructions for an "
             "agent working here. Read the build files, tests and existing "
             "docs first, and keep it to the commands, layout and conventions "
             "this project actually uses. Update the file if it exists.";
    case SlashCommandId::kReview:
      return "Review " +
             (command.argument.empty() ? std::string("my uncommitted changes")
                                       : command.argument) +
             " and find issues: defects first, then missing tests and "
             "anything that contradicts this project's conventions. Cite "
             "path:line and report the most severe finding first.";
    case SlashCommandId::kDiff:
      return "Show my current changes: run `git --no-pager diff --stat`, "
             "`git --no-pager diff` and `git status --short` (untracked files "
             "included), and report the output. Change nothing.";
    default:
      return {};
  }
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

void SetInteractiveReadHandler(InteractiveReadHandler handler) {
  ReadHandler() = std::move(handler);
}

bool InteractiveReadAvailable() { return static_cast<bool>(ReadHandler()); }

std::string InputPrompt(const char* label) {
  return std::string(CYAN()) +
         (label && *label ? std::string(label) + "> " : "> ") + RST();
}

std::string UserEchoRow(const std::string& prompt, const std::string& text) {
  // InputPrompt ends in RST(), which clears the background, so the band is
  // re-armed after it. A row the text wraps onto fills itself; one it ends
  // with a real newline does not, so each newline closes and reopens the band.
  // The composer echoes a glyph instead of a newline, so that loop only does
  // anything on the reprint path, where the raw text is passed through.
  std::string row = "\r" + std::string(InputBg()) + prompt + InputBg();
  for (size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '\n') {
      row += EraseToEol();
      row += '\n';
      row += InputBg();
    } else {
      row += text[at];
    }
  }
  return row + EraseToEol() + RST();
}

std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history, const std::string& initial) {
  return ReadInteraction(
      {.prompt = prompt, .keep_history = keep_history, .initial = initial},
      eof);
}

std::string ReadInteraction(InteractionRequest request, bool* eof) {
  static std::atomic<uint64_t> sequence{0};
  if (request.id.empty()) {
    request.id = "interaction-" + std::to_string(sequence.fetch_add(1) + 1);
  }
  *eof = false;
  Emit(Event{EventId::kInteractionRequested,
             {{"id", request.id},
              {"kind", request.kind},
              {"prompt", request.prompt},
              {"initial", request.initial},
              {"options", request.options}}});
  std::string answer;
  if (ReadHandler()) {
    answer = ReadHandler()(request, eof);
  } else {
    ScopedCookedInput cooked_input;
    fputs(request.prompt.c_str(), stdout);
    fputs(request.initial.c_str(), stdout);
    fflush(stdout);
    if (!std::getline(std::cin, answer)) {
      *eof = true;
      answer.clear();
    } else {
      answer = request.initial + answer;
    }
    fputs(RST(), stdout);
  }
  Emit(Event{EventId::kInteractionResolved,
             {{"id", request.id},
              {"kind", request.kind},
              {"eof", *eof},
              {"answer", answer}}});
  return answer;
}

std::string ReadChoiceLine(const std::string& prompt, bool& cancelled,
                           bool& eof) {
  return ReadChoiceLine({.kind = "choice", .prompt = prompt}, cancelled, eof);
}

std::string ReadChoiceLine(InteractionRequest request, bool& cancelled,
                           bool& eof) {
  std::string input = Trim(ReadInteraction(std::move(request), &eof));
  cancelled = eof || input.find('\x1b') != std::string::npos;
  return cancelled ? "" : input;
}

}  // namespace uagent
