// Copyright 2026 Timon Gentzsch

#include "include/app/options.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/strings.h"

namespace uagent {
namespace {

// One table drives both parsing and `--help`, so a flag cannot be accepted
// without being documented or documented without being accepted.
enum class FlagKind {
  kToggle,  // sets a bool on Options
  kConfig,  // takes a value, forwarded to the config layer as `key`
  kBudget,  // takes a validated dollar amount
  kPrompt,  // takes the headless prompt
  kAttach,  // takes a path, repeatable
  kVersion,
  kHelp,
};

struct FlagSpec {
  std::string_view flag;
  FlagKind kind;
  bool Options::* toggle = nullptr;
  const char* key = nullptr;    // config key for kConfig
  const char* value = nullptr;  // metavar; nullptr means the flag takes none
  const char* help = nullptr;   // empty help hides an alias from the listing
};

constexpr FlagSpec kFlags[] = {
    {"-p", FlagKind::kPrompt, nullptr, nullptr, "PROMPT",
     "run one turn, print only the final answer, exit"},
    {"--yolo", FlagKind::kToggle, &Options::yolo, nullptr, nullptr,
     "approve every requested mutation automatically"},
    {"--json", FlagKind::kToggle, &Options::json, nullptr, nullptr,
     "emit a stable JSON envelope in headless mode"},
    {"--json-stream", FlagKind::kToggle, &Options::json_stream, nullptr,
     nullptr, "emit versioned JSONL events in headless mode"},
    {"--budget", FlagKind::kBudget, nullptr, nullptr, "USD",
     "cap total session spend between model calls"},
    {"--no-memory", FlagKind::kToggle, &Options::no_memory, nullptr, nullptr,
     "disable memory recall and writes for this session"},
    {"--model", FlagKind::kConfig, nullptr, "UAGENT_MODEL", "SELECTION",
     "conversation model as [provider/]model[:variant][:effort]"},
    {"--advisor", FlagKind::kConfig, nullptr, "UAGENT_ADVISOR_MODEL",
     "SELECTION", "enable the advisor tool on this model route"},
    {"--image-model", FlagKind::kConfig, nullptr, "UAGENT_IMAGE_MODEL",
     "SELECTION", "read attached images with this model route"},
    {"--subagent-model", FlagKind::kConfig, nullptr, "UAGENT_SUBAGENT_MODEL",
     "SELECTION", "default model route for delegated subagents"},
    {"--web-search-model", FlagKind::kConfig, nullptr,
     "UAGENT_WEB_SEARCH_MODEL", "SELECTION", "model route for web search"},
    {"--memory-model", FlagKind::kConfig, nullptr, "UAGENT_MEMORY_MODEL",
     "SELECTION", "model route for background memory extraction"},
    {"--debug", FlagKind::kToggle, &Options::debug, nullptr, nullptr,
     "write a sensitive reconstructable JSONL trace"},
    {"--attach", FlagKind::kAttach, nullptr, nullptr, "PATH",
     "send an image or document with the first message"},
    {"-c", FlagKind::kToggle, &Options::resume_latest, nullptr, nullptr,
     "resume the most recent saved session"},
    {"--continue", FlagKind::kToggle, &Options::resume_latest, nullptr, nullptr,
     ""},
    {"--resume", FlagKind::kToggle, &Options::resume_pick, nullptr, nullptr,
     "pick a saved session to resume at startup"},
    {"--version", FlagKind::kVersion, nullptr, nullptr, nullptr,
     "print the installed version"},
    {"--trust-project-config", FlagKind::kToggle, &Options::trust_project,
     nullptr, nullptr, "allow this workspace's .mcp.json and .uagent/.config"},
    {"-h", FlagKind::kHelp, nullptr, nullptr, nullptr, ""},
    {"--help", FlagKind::kHelp, nullptr, nullptr, nullptr, "show this help"},
};

const FlagSpec* FindFlag(std::string_view argument) {
  auto found =
      std::find_if(std::begin(kFlags), std::end(kFlags),
                   [&](const FlagSpec& spec) { return spec.flag == argument; });
  return found == std::end(kFlags) ? nullptr : &*found;
}

}  // namespace

ParsedOptions ParseOptions(int argc, char* const argv[]) {
  ParsedOptions parsed;
  for (int index = 1; index < argc; ++index) {
    std::string argument = argv[index];
    const FlagSpec* spec = FindFlag(argument);
    if (!spec) {
      if (argument.starts_with("--debug=")) {
        parsed.options.debug = true;
        parsed.options.debug_path = argument.substr(8);
        continue;
      }
      parsed.error = "unknown flag: " + argument;
      return parsed;
    }
    std::string value;
    if (spec->value) {
      if (++index >= argc) {
        parsed.error = argument + " requires a value";
        return parsed;
      }
      value = argv[index];
    }
    switch (spec->kind) {
      case FlagKind::kToggle:
        parsed.options.*spec->toggle = true;
        break;
      case FlagKind::kConfig:
        if (Trim(value).empty()) {
          parsed.error = argument + " requires a value";
          return parsed;
        }
        parsed.options.overrides[spec->key] = Trim(value);
        break;
      case FlagKind::kBudget:
        if (!ParseFiniteDouble(value.c_str(), parsed.options.budget) ||
            parsed.options.budget <= 0) {
          parsed.error = "--budget must be a positive dollar amount";
          return parsed;
        }
        break;
      case FlagKind::kPrompt:
        parsed.options.prompt = std::move(value);
        break;
      case FlagKind::kAttach:
        parsed.options.attach_paths.push_back(std::move(value));
        break;
      case FlagKind::kVersion:
        parsed.action = OptionsAction::kVersion;
        return parsed;
      case FlagKind::kHelp:
        parsed.action = OptionsAction::kHelp;
        return parsed;
    }
  }
  if (parsed.options.budget > 0) {
    parsed.options.overrides["UAGENT_SESSION_BUDGET"] =
        std::to_string(parsed.options.budget);
  }
  if (parsed.options.no_memory) parsed.options.overrides["UAGENT_MEMORY"] = "0";
  if (parsed.options.json && parsed.options.json_stream) {
    parsed.error = "--json and --json-stream are mutually exclusive";
  } else if ((parsed.options.json || parsed.options.json_stream) &&
             parsed.options.prompt.empty()) {
    parsed.error = "JSON output requires -p PROMPT";
  }
  return parsed;
}

const char* UsageText() {
  static const std::string text = [] {
    auto invocation_of = [](const FlagSpec& spec) {
      // --debug is spelled with its optional value in both places.
      if (spec.flag == "--debug") return std::string("--debug[=PATH]");
      std::string invocation(spec.flag);
      if (spec.value) invocation += " " + std::string(spec.value);
      return invocation;
    };
    size_t column = 0;
    for (const FlagSpec& spec : kFlags) {
      if (!spec.help || !*spec.help) continue;
      column = std::max(column, invocation_of(spec).size() + 4);
    }
    std::string synopsis = "usage: uagent";
    std::string listing;
    for (const FlagSpec& spec : kFlags) {
      if (!spec.help || !*spec.help) continue;
      std::string invocation = invocation_of(spec);
      synopsis += " [" + invocation + "]";
      std::string row = "  " + invocation;
      row.append(column - row.size(), ' ');
      listing += row + spec.help + "\n";
    }
    return synopsis + "\n\n" + listing +
           "\nconfig: ./.uagent/.config when trusted, then ~/.uagent/.config; "
           "process UAGENT_* variables override both, and the flags above "
           "override all three\n";
  }();
  return text.c_str();
}

}  // namespace uagent
