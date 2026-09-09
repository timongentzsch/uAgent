// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CLI_H_
#define UAGENT_INCLUDE_CLI_H_
// Terminal input and slash-command metadata.

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "include/core/json.h"

namespace uagent {

enum class SlashCommandId {
  kAgents,
  kAttach,
  kCompact,
  kContext,
  kConfig,
  kFork,
  kPermissions,
  kPrompt,
  kHttp,
  kDiff,
  kEffort,
  kHelp,
  kInit,
  kMemory,
  kSkills,
  kSchedule,
  kModel,
  kModels,
  kCost,
  kProcesses,
  kQuit,
  kReset,
  kReview,
  kSessions,
  kStatus,
  kTools,
  kTrace,
  kDebugConfig,
  kVariant,
  kVerbose,
  kYolo,
};

struct SlashCommandSpec {
  SlashCommandId id;
  const char* name;
  const char* argument;
  const char* description;
};

struct ParsedSlashCommand {
  const SlashCommandSpec* spec = nullptr;
  std::string argument;
};

ParsedSlashCommand ParseSlashCommand(const std::string& input);
// The turn a prompt command stands for; empty for a local action.
std::string SlashCommandPrompt(const ParsedSlashCommand& command);
void PrintCommandHelp();
// The same rows the parser and help output use; an alias carries an empty
// description and is hidden from listings.
std::span<const SlashCommandSpec> SlashCommandRegistry();

enum class InteractiveInputKind { kNone, kLine, kEscape, kBackground, kEof };

struct InteractiveInputEvent {
  InteractiveInputKind kind = InteractiveInputKind::kNone;
  std::string text;
};

struct InteractionRequest {
  std::string id = "";
  std::string kind = "text";
  std::string prompt;
  bool keep_history = false;
  std::string initial = "";
  json options = json::array();
};

using InteractiveReadHandler =
    std::function<std::string(const InteractionRequest&, bool*)>;
void SetInteractiveReadHandler(InteractiveReadHandler handler);
bool InteractiveReadAvailable();

std::string InputPrompt(const char* label = "");
// One echoed user turn, banded to the right edge. `text` is already display-
// ready: callers differ in how they sanitize it, and the composer's mapping of
// newlines to a glyph is what keeps the echo on the rows it drew.
std::string UserEchoRow(const std::string& prompt, const std::string& text);
std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history = true,
                          const std::string& initial = "");
std::string ReadInteraction(InteractionRequest request, bool* eof);
std::string ReadChoiceLine(const std::string& prompt, bool& cancelled,
                           bool& eof);
std::string ReadChoiceLine(InteractionRequest request, bool& cancelled,
                           bool& eof);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CLI_H_
