// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CLI_H_
#define UAGENT_INCLUDE_CLI_H_
// Terminal input and slash-command metadata. State and implementation live in
// cli.cc so libedit build flags cannot change header-owned process state.

#include <string>
#include <vector>

namespace uagent {

enum class CommandCompletion { kNone, kFilenames, kModels, kEfforts };
enum class SlashCommandId {
  kAttach,
  kCompact,
  kContext,
  kDetach,
  kEffort,
  kHelp,
  kHandoff,
  kModel,
  kModels,
  kCost,
  kOnline,
  kQuit,
  kReset,
  kSessions,
  kTrace,
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

#if defined(HAVE_EDITLINE)
void RegisterCompletion(CommandCompletion source, const std::string& value);
void ConfigureCompletion(const std::vector<std::string>& models,
                         const std::vector<std::string>& efforts);
#endif

std::string InputPrompt(const char* label = "");
std::string ReadInputLine(const std::string& prompt, bool* eof,
                          bool keep_history = true,
                          const std::string& initial = "");
std::string ReadChoiceLine(const std::string& prompt, bool& cancelled,
                           bool& eof);
std::string SteeringReplacement(bool& cancelled);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CLI_H_
