// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_OPTIONS_H_
#define UAGENT_INCLUDE_APP_OPTIONS_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/env.h"

namespace uagent {

enum class OptionsAction {
  kRun,
  kHelp,
  kPrintVersion,
  kEmitReference,
};

struct Options {
  bool web = false;
  bool show_system_prompt = false;
  bool yolo = false;
  bool trust_project = false;
  bool debug = false;
  bool json = false;
  bool json_stream = false;
  bool resume_latest = false;
  bool resume_pick = false;
  std::string debug_path;
  std::string reference_dir;
  std::string prompt;
  std::string control;
  std::vector<std::string> attach_paths;
  // UAGENT_* values named on the command line; they outrank the environment
  // and both config files. --budget and --no-memory land here too.
  RuntimeConfig::Values overrides;
};

// One table drives parsing, `--help` and the generated CLI reference, so a flag
// cannot be accepted without being documented or documented without being
// accepted.
enum class FlagKind {
  kToggle,       // sets a bool on Options
  kConfig,       // takes a value, forwarded to the config layer as `key`
  kConfigSet,    // takes none, forwards the fixed `preset` as `key`
  kBudget,       // takes a validated dollar amount
  kTokenBudget,  // takes a validated generated-token count
  kPrompt,       // takes the headless prompt
  kControl,      // a native management JSON request; - reads stdin
  kAttach,       // takes a path, repeatable
  kPrintVersion,
  kEmitReference,
  kHelp,
};

struct FlagSpec {
  std::string_view flag;
  FlagKind kind;
  bool Options::* toggle = nullptr;
  const char* key = nullptr;    // config key for kConfig/kConfigSet
  const char* value = nullptr;  // metavar; nullptr means the flag takes none
  const char* help = nullptr;   // empty help hides an alias from the listing
  // The value is written as `--flag=VALUE` instead of a separate argument, is
  // optional, and lands in `text` rather than the config layer.
  bool optional_value = false;
  std::string Options::* text = nullptr;
  const char* preset = nullptr;  // config value for kConfigSet
};

struct ParsedOptions {
  Options options;
  OptionsAction action = OptionsAction::kRun;
  std::string error;

  bool Ok() const { return error.empty(); }
};

ParsedOptions ParseOptions(int argc, char* const argv[]);
const char* UsageText();
std::span<const FlagSpec> FlagRegistry();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_OPTIONS_H_
