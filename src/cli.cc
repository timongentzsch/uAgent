// Copyright 2026 Timon Gentzsch

#include "include/cli.h"

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
}  // namespace

constexpr SlashCommandSpec kSlashCommands[] = {
    {SlashCommandId::kAttach, "/attach", "PATH|clear",
     "attach a file to the next turn"},
    {SlashCommandId::kCompact, "/compact", "", "summarize active context"},
    {SlashCommandId::kContext, "/context", "", "show current model request"},
    {SlashCommandId::kCost, "/cost", "", "show spend by model route"},
    {SlashCommandId::kEffort, "/effort", "LEVEL", "set reasoning effort"},
    {SlashCommandId::kHelp, "/help", "", "show this help"},
    {SlashCommandId::kHandoff, "/handoff", "PROVIDER/MODEL",
     "checkpoint and switch route"},
    {SlashCommandId::kMemory, "/memory", "", "show memory state and keys"},
    {SlashCommandId::kModel, "/model", "NAME", "switch model or route"},
    {SlashCommandId::kModels, "/models", "[QUERY]",
     "search and select across providers"},
    {SlashCommandId::kOnline, "/online", "", "toggle OpenRouter web search"},
    {SlashCommandId::kProcesses, "/ps", "", "show active background work"},
    {SlashCommandId::kQuit, "/quit", "", "exit µAgent"},
    {SlashCommandId::kReset, "/reset", "", "start a fresh session"},
    {SlashCommandId::kSessions, "/sessions", "", "resume a saved session"},
    {SlashCommandId::kTrace, "/trace", "", "show latest tool and search trace"},
    {SlashCommandId::kVariant, "/variant", "MODE",
     "set OpenRouter provider routing"},
    {SlashCommandId::kVerbose, "/verbose", "", "toggle full tool traces"},
    {SlashCommandId::kYolo, "/yolo", "", "toggle automatic approval"},
    {SlashCommandId::kQuit, "/exit", "", ""},
    {SlashCommandId::kQuit, "/q", "", ""},
    {SlashCommandId::kReset, "/clear", "", ""},
    {SlashCommandId::kReset, "/new", "", ""},
    {SlashCommandId::kContext, "/ctx", "", ""},
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
