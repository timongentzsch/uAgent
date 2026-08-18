// Copyright 2026 Timon Gentzsch

#include "include/cli.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

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
    {SlashCommandId::kCompact, "/compact", "", "summarize active context"},
    {SlashCommandId::kContext, "/context", "", "show current model request"},
    {SlashCommandId::kCost, "/cost", "", "show tokens and spend by route"},
    {SlashCommandId::kEffort, "/effort", "LEVEL", "set reasoning effort"},
    {SlashCommandId::kHelp, "/help", "", "show this help"},
    {SlashCommandId::kMemory, "/memory", "", "show memory state and keys"},
    {SlashCommandId::kModel, "/model", "NAME", "switch model or route"},
    {SlashCommandId::kModels, "/models", "[QUERY]",
     "search and select across providers"},
    {SlashCommandId::kProcesses, "/ps", "", "show active background work"},
    {SlashCommandId::kQuit, "/quit", "", "exit µAgent"},
    {SlashCommandId::kReset, "/reset", "", "start a fresh session"},
    {SlashCommandId::kSessions, "/sessions", "", "resume a saved session"},
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
};

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

std::string InputPrompt(const char* label) {
  return std::string(CYAN()) +
         (label && *label ? std::string(label) + "> " : "> ") + RST();
}

std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history, const std::string& initial) {
  *eof = false;
  if (ReadHandler()) {
    return ReadHandler()(prompt, eof, keep_history, initial);
  }
  ScopedCookedInput cooked_input;
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
  std::string input = Trim(ReadInputLine(prompt, &eof, false));
  cancelled = eof || input.find('\x1b') != std::string::npos;
  return cancelled ? "" : input;
}

}  // namespace uagent
