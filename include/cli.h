// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CLI_H_
#define UAGENT_INCLUDE_CLI_H_
// Terminal input and slash-command metadata. State and implementation live in
// cli.cc so libedit build flags cannot change header-owned process state.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace uagent {

enum class CommandCompletion {
  kNone,
  kFilenames,
  kModels,
  kEfforts,
  kVariants
};
enum class SlashCommandId {
  kAttach,
  kCompact,
  kContext,
  kEffort,
  kHelp,
  kHandoff,
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
  CommandCompletion completion;
};

struct ParsedSlashCommand {
  const SlashCommandSpec* spec = nullptr;
  std::string argument;
};

const SlashCommandSpec* SlashCommand(const std::string& name);
ParsedSlashCommand ParseSlashCommand(const std::string& input);
void PrintCommandHelp();
void ConfigureLineEditor();

enum class InteractiveInputKind { kNone, kLine, kEscape, kEof };

struct InteractiveInputEvent {
  InteractiveInputKind kind = InteractiveInputKind::kNone;
  std::string text;
};

using InteractiveReadHandler = std::function<std::string(
    const std::string&, bool*, bool, const std::string&)>;
void SetInteractiveReadHandler(InteractiveReadHandler handler);

#if defined(HAVE_EDITLINE)
void RegisterCompletion(CommandCompletion source, const std::string& value);
void ConfigureCompletion(const std::vector<std::string>& models,
                         const std::vector<std::string>& efforts,
                         const std::vector<std::string>& variants);
#endif

std::string InputPrompt(const char* label = "");
std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history = true,
                          const std::string& initial = "");
std::string ReadChoiceLine(const std::string& prompt, bool& cancelled,
                           bool& eof);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CLI_H_
