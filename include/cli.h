// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CLI_H_
#define UAGENT_INCLUDE_CLI_H_
// Terminal input and slash-command metadata. Commands, help, parsing, and
// completion all derive from one registry.

#include <poll.h>
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
#include <vector>

#if defined(HAVE_EDITLINE)
#include <editline/readline.h>
#endif

#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

enum class CommandCompletion { kNone, kFilenames, kModels, kEfforts };
enum class SlashCommandId {
  kAttach,
  kCompact,
  kDetach,
  kEffort,
  kHelp,
  kModel,
  kModels,
  kOnline,
  kQuit,
  kReset,
  kSessions,
  kTrace,
  kYolo,
};

struct SlashCommandSpec {
  SlashCommandId id;
  const char* name;
  const char* argument;
  const char* description;
  CommandCompletion completion;
};

inline constexpr SlashCommandSpec kSlashCommands[] = {
    {SlashCommandId::kAttach, "/attach", "PATH",
     "attach a file to the next turn", CommandCompletion::kFilenames},
    {SlashCommandId::kCompact, "/compact", "", "summarize active context",
     CommandCompletion::kNone},
    {SlashCommandId::kDetach, "/detach", "", "clear pending attachments",
     CommandCompletion::kNone},
    {SlashCommandId::kEffort, "/effort", "LEVEL", "set reasoning effort",
     CommandCompletion::kEfforts},
    {SlashCommandId::kHelp, "/help", "", "show this help",
     CommandCompletion::kNone},
    {SlashCommandId::kModel, "/model", "NAME", "switch model or provider",
     CommandCompletion::kModels},
    {SlashCommandId::kModels, "/models", "[TARGET]",
     "query provider, filter, or all", CommandCompletion::kNone},
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
    {SlashCommandId::kYolo, "/yolo", "", "toggle automatic approval",
     CommandCompletion::kNone},
    {SlashCommandId::kQuit, "/exit", "", "", CommandCompletion::kNone},
    {SlashCommandId::kQuit, "/q", "", "", CommandCompletion::kNone},
    {SlashCommandId::kReset, "/clear", "", "", CommandCompletion::kNone},
    {SlashCommandId::kReset, "/new", "", "", CommandCompletion::kNone},
};

inline const SlashCommandSpec* SlashCommand(const std::string& name) {
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (name == command.name) return &command;
  }
  return nullptr;
}

struct ParsedSlashCommand {
  const SlashCommandSpec* spec = nullptr;
  std::string argument;
};

inline ParsedSlashCommand ParseSlashCommand(const std::string& input) {
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (input == command.name) return {&command, ""};
    std::string prefix = std::string(command.name) + " ";
    if (*command.argument && input.starts_with(prefix)) {
      return {&command, Trim(input.substr(prefix.size()))};
    }
  }
  return {};
}

inline void PrintCommandHelp() {
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
inline std::queue<unsigned char>
    readline_pending;  // bytes read ahead, replayed first
inline std::string readline_initial;
inline size_t readline_initial_pos = 0;
inline bool steering_prompt = false, steering_cancelled = false;
inline std::vector<std::string> readline_commands, readline_models,
    readline_efforts;
inline const std::vector<std::string>* readline_candidates = nullptr;

inline char* CompletionCandidate(const char* text, int state) {
  static size_t index;
  if (!state) index = 0;
  while (readline_candidates && index < readline_candidates->size()) {
    const std::string& candidate = (*readline_candidates)[index++];
    if (candidate.starts_with(text)) return strdup(candidate.c_str());
  }
  return nullptr;
}

inline char** UagentCompletion(const char* text, int start, int) {
  rl_attempted_completion_over = 1;
  readline_candidates = nullptr;
  std::string before = rl_line_buffer ? std::string(rl_line_buffer, start) : "";
  if (start == 0 && *text == '/') {
    readline_candidates = &readline_commands;
  } else if (const SlashCommandSpec* command = SlashCommand(Trim(before))) {
    if (command->completion == CommandCompletion::kModels) {
      readline_candidates = &readline_models;
    } else if (command->completion == CommandCompletion::kEfforts) {
      readline_candidates = &readline_efforts;
    } else if (command->completion == CommandCompletion::kFilenames) {
      rl_attempted_completion_over = 0;
      return nullptr;
    }
  } else if (before.empty() || before[0] != '/') {
    rl_attempted_completion_over = 0;
    return nullptr;
  }
  // libedit uses `char*` here on Linux and `const char*` on macOS.
  return readline_candidates
             ? completion_matches(const_cast<char*>(text), CompletionCandidate)
             : nullptr;
}

inline int ReadlineGetc(FILE* file) {
  if (readline_initial_pos < readline_initial.size()) {
    return static_cast<unsigned char>(readline_initial[readline_initial_pos++]);
  }
  if (!readline_pending.empty()) {
    int ch = readline_pending.front();
    readline_pending.pop();
    return ch;
  }
  int fd = file ? fileno(file) : 0;
  unsigned char ch;
  ssize_t size;
  do {
    size = read(fd, &ch, 1);
  } while (size < 0 && errno == EINTR);
  return size > 0 ? ch : EOF;
}

inline int EscGetc(FILE* file) {
  int ch = ReadlineGetc(file);
  if (ch == 0x1b) {
    int fd = file ? fileno(file) : 0;
    pollfd event{fd, POLLIN, 0};
    if (poll(&event, 1, 50) == 0) {
      if (steering_prompt) {
        steering_cancelled = true;
        return '\n';
      }
      readline_pending.push(0x0b);
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
    for (unsigned char byte : sequence) readline_pending.push(byte);
  }
  return ch;
}

inline void RegisterCompletion(CommandCompletion source,
                               const std::string& value) {
  std::vector<std::string>* values = nullptr;
  if (source == CommandCompletion::kModels) values = &readline_models;
  if (source == CommandCompletion::kEfforts) values = &readline_efforts;
  if (!values || value.empty() ||
      std::find(values->begin(), values->end(), value) != values->end()) {
    return;
  }
  values->push_back(value);
}

inline void ConfigureCompletion(const std::vector<std::string>& models,
                                const std::vector<std::string>& efforts) {
  readline_commands.clear();
  readline_models = models;
  readline_efforts = efforts;
  for (const SlashCommandSpec& command : kSlashCommands) {
    if (*command.description) readline_commands.push_back(command.name);
  }
  std::sort(readline_models.begin(), readline_models.end());
  std::sort(readline_efforts.begin(), readline_efforts.end());
  readline_models.erase(
      std::unique(readline_models.begin(), readline_models.end()),
      readline_models.end());
  readline_efforts.erase(
      std::unique(readline_efforts.begin(), readline_efforts.end()),
      readline_efforts.end());
  rl_attempted_completion_function = UagentCompletion;
}

inline std::string RlHideEscapes(const std::string& prompt) {
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

inline std::string PanelPrompt(const char* label = "") {
  if (!g_tty) return std::string(label) + "> ";
  return std::string(PANEL()) +
         (label && *label ? std::string(label) + "> " : "> ");
}

inline std::string ReadInputLine(const std::string& prompt, bool* eof,
                                 bool keep_history = true,
                                 const std::string& initial = "") {
  (void)keep_history;  // used only when libedit is compiled in
  *eof = false;
#if defined(HAVE_EDITLINE)
  if (g_tty) {
    readline_initial = initial;
    readline_initial_pos = 0;
    std::string hidden_prompt = RlHideEscapes(prompt);
    BracketedPaste(true);
    char* line = readline(hidden_prompt.c_str());
    BracketedPaste(false);
    readline_initial.clear();
    readline_initial_pos = 0;
    fputs(RST(), stdout);
    TerminalClearToEnd();
    if (!line) {
      *eof = true;
      return "";
    }
    std::string input = line;
    free(line);
    if (keep_history && !Trim(input).empty()) add_history(input.c_str());
    return input;
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

inline std::string SteeringReplacement(bool& cancelled) {
  std::string input;
  do {
    bool eof = false;
    PanelClearLine();
#if defined(HAVE_EDITLINE)
    steering_prompt = true;
    steering_cancelled = false;
#endif
    input = Trim(ReadInputLine(PanelPrompt("steer"), &eof, false));
#if defined(HAVE_EDITLINE)
    cancelled = steering_cancelled;
    steering_prompt = steering_cancelled = false;
    if (!cancelled && !input.empty()) add_history(input.c_str());
#else
    cancelled = input.find('\x1b') != std::string::npos;
#endif
    if (eof) cancelled = true;
  } while (!cancelled && input.empty());
  return cancelled ? "" : input;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CLI_H_
