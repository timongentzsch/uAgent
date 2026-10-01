// Copyright 2026 Timon Gentzsch

#include "include/cli.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/events.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/editor.h"
#include "include/ui/presentation.h"

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
    {SlashCommandId::kAgents, "/agents",
     "[ID [output|stop|message TEXT|followup TEXT]]",
     "inspect, guide or resume delegated agents", kTerminal},
    {SlashCommandId::kAttach, "/attach", "PATH|clear",
     "attach a file to the next turn", kNoViewer | kTerminal},
    {SlashCommandId::kCompact, "/compact", "",
     "summarize conversation to prevent hitting the context limit", kNoViewer},
    {SlashCommandId::kContext, "/context", "", "show current model request"},
    {SlashCommandId::kConfig, "/config",
     "[user|project KEY=VALUE|unset KEY|reset]",
     "show changed settings, change one, or reset a scope"},
    {SlashCommandId::kFork, "/fork", "[TITLE] [@TURN]",
     "branch this conversation, optionally at user turn N",
     kNoViewer | kClientOnly},
    {SlashCommandId::kRewind, "/rewind", "[N]",
     "fork before your message N to edit it; bare, list the numbers",
     kNoViewer},
    {SlashCommandId::kShare, "/share", "", "export transcript as markdown",
     kNoViewer | kClientOnly},
    {SlashCommandId::kPermissions, "/permissions",
     "[default|ask|auto|yolo|rules|forget N|forget all]",
     "show or change permission mode, or this repository's remembered "
     "actions",
     kNoViewer},
    {SlashCommandId::kRename, "/rename", "TITLE", "rename this conversation",
     kNoViewer},
    {SlashCommandId::kInstructions, "/instructions",
     "[edit sessions|coordinator user|project | clear]",
     "show or edit instructions; clear this conversation's self-directive"},
    {SlashCommandId::kHttp, "/http", "[INDEX [request|response]]",
     "inspect captured HTTP attempts (latest by default)"},
    {SlashCommandId::kDiff, "/diff", "",
     "show git diff (including untracked files)"},
    {SlashCommandId::kCost, "/cost", "", "show tokens and spend by route",
     kTerminal},
    {SlashCommandId::kChanges, "/changes", "",
     "list the files the last turn changed", kTerminal},
    {SlashCommandId::kUndo, "/undo", "[FILE]",
     "put back the files the last turn changed, or one of them",
     kNoViewer | kTerminal},
    {SlashCommandId::kDebugConfig, "/debug-config", "[SETTING]",
     "show configuration layers, sources and restart-required fields",
     kTerminal},
    {SlashCommandId::kEffort, "/effort", "LEVEL",
     "choose how much reasoning effort to use", kNoViewer | kTerminal},
    {SlashCommandId::kHelp, "/help", "", "show this help", kTerminal},
    {SlashCommandId::kInit, "/init", "",
     "create an AGENTS.md file with instructions for \u00b5Agent"},
    {SlashCommandId::kLink, "/link", "[TOKEN]",
     "create a session link or join one with its token", kTerminal},
    {SlashCommandId::kMemory, "/memory",
     "[list|get KEY|set KEY @FILE|forget KEY|rename KEY TARGET|copy KEY "
     "TARGET]",
     "manage project and global memories"},
    {SlashCommandId::kSkills, "/skills",
     "[list|get ID|set KEY @FILE|forget ID|enable ID|disable ID]",
     "inspect and manage installed skills"},
    {SlashCommandId::kSchedule, "/schedule", "[list|JSON]",
     "manage scheduled tasks and runs"},
    {SlashCommandId::kModel, "/model", "NAME", "choose what model to use",
     kNoViewer | kTerminal},
    {SlashCommandId::kModels, "/models", "[QUERY]",
     "search and select across providers", kTerminal},
    {SlashCommandId::kProcesses, "/ps", "[ID [output|stop]]",
     "inspect or stop background work", kTerminal},
    {SlashCommandId::kPeers, "/peers", "", "list linked and linkable sessions",
     kTerminal},
    {SlashCommandId::kQuit, "/quit", "", "exit uagent",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kReset, "/reset", "", "start a new chat",
     kNoViewer | kClientOnly},
    {SlashCommandId::kReset, "/new", "", "start a new chat",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kClear, "/clear", "", "clear the screen",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kReview, "/review", "[TARGET]",
     "review my current changes and find issues"},
    {SlashCommandId::kSessions, "/sessions", "[PREFIX]",
     "resume a saved chat, optionally matching PREFIX",
     kClientOnly | kTerminal},
    {SlashCommandId::kStatus, "/status", "",
     "show current session configuration and token usage"},
    {SlashCommandId::kTools, "/tools", "[on|off NAME|profile NAME|reset]",
     "inspect or choose tools for this conversation"},
    {SlashCommandId::kMcp, "/mcp", "[retry|on|off NAME]",
     "show MCP servers; retry one or switch it on or off"},
    {SlashCommandId::kRestart, "/restart", "",
     "restart this conversation to apply settings that need it",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kVariant, "/variant", "MODE",
     "set OpenRouter provider routing", kNoViewer | kTerminal},
    {SlashCommandId::kBtw, "/btw", "QUESTION",
     "ask a side question without adding it to the conversation",
     kNoViewer | kClientOnly},
    {SlashCommandId::kVerbose, "/verbose", "",
     "toggle full reasoning and expanded tool output",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kYolo, "/yolo", "", "toggle automatic approval",
     kNoViewer | kTerminal},
    {SlashCommandId::kCoord, "/coord", "", "open this folder's coordinator",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kBoard, "/board", "",
     "list this folder's sessions and threads",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kOpen, "/open", "ID", "switch to a session from /board",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kHelp, "/commands", "", "", kTerminal},
    {SlashCommandId::kQuit, "/exit", "", "",
     kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kQuit, "/q", "", "", kNoViewer | kClientOnly | kTerminal},
    {SlashCommandId::kContext, "/ctx", "", ""},
    {SlashCommandId::kSessions, "/resume", "[PREFIX]", "",
     kClientOnly | kTerminal},
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

std::string NearestSlashCommand(const std::string& input) {
  const std::string word = input.substr(0, input.find(' '));
  std::string nearest;
  // More than two edits, or more than half the letters, is another word.
  size_t best = 3;
  for (const SlashCommandSpec& command : kSlashCommands) {
    // Levenshtein over one rolling row; the names are a few bytes long.
    const std::string_view name = command.name;
    std::vector<size_t> row(name.size() + 1);
    for (size_t j = 0; j <= name.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= word.size(); ++i) {
      size_t diagonal = row[0];
      row[0] = i;
      for (size_t j = 1; j <= name.size(); ++j) {
        const size_t above = row[j];
        row[j] = std::min({above + 1, row[j - 1] + 1,
                           diagonal + (word[i - 1] != name[j - 1])});
        diagonal = above;
      }
    }
    if (row.back() < best && 2 * row.back() + 1 <= word.size()) {
      best = row.back();
      nearest = name;
    }
  }
  return nearest;
}

ForkArgument ParseForkArgument(const std::string& argument) {
  const std::string rest = Trim(argument);
  auto number = [](const std::string& text) {
    return !text.empty() && text.size() <= 9 &&
           std::all_of(text.begin(), text.end(), ::isdigit);
  };
  ForkArgument fork{rest};
  size_t at = rest.rfind(" @");
  if (at == std::string::npos && rest.starts_with('@')) at = 0;
  if (at != std::string::npos) {
    const std::string tail = Trim(rest.substr(at + (at == 0 ? 1 : 2)));
    if (number(tail)) fork = {Trim(rest.substr(0, at)), std::stoll(tail)};
  } else if (number(rest)) {
    fork = {"", std::stoll(rest)};
  }
  return fork;
}

void SetInteractiveReadHandler(InteractiveReadHandler handler) {
  ReadHandler() = std::move(handler);
}

bool InteractiveReadAvailable() { return static_cast<bool>(ReadHandler()); }

// Plain mode names the speaker, as it names every other row.
std::string InputPrompt() {
  return std::string(BOLD()) + (g_plain ? "you: " : "> ") + RST();
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
  // Terminals already stamp scrollback lines; the stored per-message time
  // stays in the display facts for the browser.
  return row + EraseToEol() + RST();
}

std::string DecisionPrompt(const std::string& prompt, const json& options) {
  std::string hints;
  bool guidance = false;
  for (const json& option : options) {
    std::string value = JsonValue(option, "value", "");
    if (value == "guidance") {
      guidance = true;
    } else if (value.size() == 1 &&
               std::isalpha(static_cast<unsigned char>(value[0]))) {
      hints += "  [" + value + "] " + JsonValue(option, "label", "");
    } else {
      return prompt;
    }
  }
  if (guidance) hints += AsciiGlyphs(" \u2014 or type what to do instead");
  return prompt + hints;
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
  } else if (request.kind == "editor") {
    ScopedCookedInput cooked_input;
    answer = request.initial;
    *eof = !EditExternalText(answer, STDIN_FILENO, kAdaptiveSystemBytes);
  } else {
    ScopedCookedInput cooked_input;
    fputs((TerminalSafe(DecisionPrompt(request.prompt, request.options)) + " ")
              .c_str(),
          stdout);
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

std::string ReadChoiceLine(InteractionRequest request, bool& cancelled,
                           bool& eof) {
  std::string input = Trim(ReadInteraction(std::move(request), &eof));
  cancelled = eof || input.find('\x1b') != std::string::npos;
  return cancelled ? "" : input;
}

}  // namespace uagent
