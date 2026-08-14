// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CLI_H_
#define UAGENT_INCLUDE_CLI_H_
// Terminal input and slash-command metadata.

#include <cstdint>
#include <functional>
#include <string>

namespace uagent {

enum class SlashCommandId {
  kAttach,
  kCompact,
  kContext,
  kEffort,
  kHelp,
  kMemory,
  kModel,
  kModels,
  kCost,
  kOnline,
  kProcesses,
  kQuit,
  kReset,
  kSessions,
  kTrace,
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
void PrintCommandHelp();

enum class InteractiveInputKind {
  kNone,
  kLine,
  kEscape,
  kBackground,
  kEof
};

struct InteractiveInputEvent {
  InteractiveInputKind kind = InteractiveInputKind::kNone;
  std::string text;
};

using InteractiveReadHandler = std::function<std::string(
    const std::string&, bool*, bool, const std::string&)>;
void SetInteractiveReadHandler(InteractiveReadHandler handler);

std::string InputPrompt(const char* label = "");
std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history = true,
                          const std::string& initial = "");
std::string ReadChoiceLine(const std::string& prompt, bool& cancelled,
                           bool& eof);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CLI_H_
