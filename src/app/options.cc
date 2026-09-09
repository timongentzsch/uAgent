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

constexpr FlagSpec kFlags[] = {
    {"--control", FlagKind::kControl, nullptr, nullptr, "JSON|-",
     "run a native memory, skills or schedule operation without a model"},
    {"--web", FlagKind::kToggle, &Options::web, nullptr, nullptr,
     "start or reuse this user's global web application"},
    {"--web-port", FlagKind::kConfig, nullptr, "UAGENT_WEB_PORT", "PORT",
     "loopback web listener port (default 8080)"},
    {"--web-origin", FlagKind::kConfig, nullptr, "UAGENT_WEB_ORIGIN", "ORIGIN",
     "stable HTTPS browser origin behind a local reverse proxy"},
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
    {"--token-budget", FlagKind::kTokenBudget, nullptr, nullptr, "TOKENS",
     "cap generated tokens across the session between model calls"},
    {.flag = "--no-memory",
     .kind = FlagKind::kConfigSet,
     .key = "UAGENT_MEMORY",
     .help = "disable memory recall and writes for this session",
     .preset = "0"},
    {"--model", FlagKind::kConfig, nullptr, "UAGENT_MODEL", "SELECTION",
     "conversation model as [provider/]model[:variant][:effort]"},
    {"--image-model", FlagKind::kConfig, nullptr, "UAGENT_IMAGE_MODEL",
     "SELECTION", "read attached images with this model route"},
    {"--subagent-model", FlagKind::kConfig, nullptr, "UAGENT_SUBAGENT_MODEL",
     "SELECTION", "default model route for delegated subagents"},
    {"--web-search-model", FlagKind::kConfig, nullptr,
     "UAGENT_WEB_SEARCH_MODEL", "SELECTION", "model route for web search"},
    {"--memory-model", FlagKind::kConfig, nullptr, "UAGENT_MEMORY_MODEL",
     "SELECTION", "model route for background memory extraction"},
    {.flag = "--debug",
     .kind = FlagKind::kToggle,
     .toggle = &Options::debug,
     .value = "PATH",
     .help = "write a sensitive reconstructable JSONL trace",
     .optional_value = true,
     .text = &Options::debug_path},
    {"--attach", FlagKind::kAttach, nullptr, nullptr, "PATH",
     "send an image or document with the first message"},
    {"-c", FlagKind::kToggle, &Options::resume_latest, nullptr, nullptr,
     "resume the most recent saved session"},
    {"--continue", FlagKind::kToggle, &Options::resume_latest, nullptr, nullptr,
     ""},
    {"--resume", FlagKind::kToggle, &Options::resume_pick, nullptr, nullptr,
     "pick a saved session to resume at startup"},
    {"--version", FlagKind::kPrintVersion, nullptr, nullptr, nullptr,
     "print the installed version"},
    // Build-time documentation generation; hidden because it is a maintainer
    // tool, not a way to run the agent.
    {.flag = "--emit-reference",
     .kind = FlagKind::kEmitReference,
     .value = "DIR",
     .help = "",
     .text = &Options::reference_dir},
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
    std::string value;
    bool has_value = false;
    const FlagSpec* spec = FindFlag(argument);
    if (!spec) {
      // `--flag=VALUE` is accepted only where the table marks the value
      // optional; every other flag spells its value as the next argument.
      size_t equals = argument.find('=');
      const FlagSpec* prefixed =
          equals == std::string::npos
              ? nullptr
              : FindFlag(std::string_view(argument).substr(0, equals));
      if (!prefixed || !prefixed->optional_value) {
        parsed.error = "unknown flag: " + argument;
        return parsed;
      }
      spec = prefixed;
      value = argument.substr(equals + 1);
      has_value = true;
    } else if (spec->value && !spec->optional_value) {
      if (++index >= argc) {
        parsed.error = argument + " requires a value";
        return parsed;
      }
      value = argv[index];
      has_value = true;
    }
    switch (spec->kind) {
      case FlagKind::kToggle:
        parsed.options.*spec->toggle = true;
        if (has_value && spec->text) parsed.options.*spec->text = value;
        break;
      case FlagKind::kConfig:
        if (Trim(value).empty()) {
          parsed.error = argument + " requires a value";
          return parsed;
        }
        parsed.options.overrides[spec->key] = Trim(value);
        break;
      case FlagKind::kConfigSet:
        parsed.options.overrides[spec->key] = spec->preset;
        break;
      case FlagKind::kBudget: {
        double budget = 0;
        if (!ParseFiniteDouble(value.c_str(), budget) || budget <= 0) {
          parsed.error = "--budget must be a positive dollar amount";
          return parsed;
        }
        parsed.options.overrides["UAGENT_SESSION_BUDGET"] =
            std::to_string(budget);
        break;
      }
      case FlagKind::kTokenBudget: {
        int64_t budget = 0;
        if (!ParseInt64(value.c_str(), budget) || budget <= 0) {
          parsed.error = "--token-budget must be a positive integer";
          return parsed;
        }
        parsed.options.overrides["UAGENT_SESSION_TOKEN_BUDGET"] =
            std::to_string(budget);
        break;
      }
      case FlagKind::kPrompt:
        parsed.options.prompt = std::move(value);
        break;
      case FlagKind::kControl:
        if (value.empty()) {
          parsed.error = "--control requires JSON or -";
          return parsed;
        }
        parsed.options.control = std::move(value);
        break;
      case FlagKind::kAttach:
        parsed.options.attach_paths.push_back(std::move(value));
        break;
      case FlagKind::kPrintVersion:
        parsed.action = OptionsAction::kPrintVersion;
        return parsed;
      case FlagKind::kEmitReference:
        parsed.options.reference_dir = std::move(value);
        parsed.action = OptionsAction::kEmitReference;
        return parsed;
      case FlagKind::kHelp:
        parsed.action = OptionsAction::kHelp;
        return parsed;
    }
  }
  if (!parsed.options.control.empty() &&
      (parsed.options.web || parsed.options.yolo ||
       parsed.options.trust_project || parsed.options.debug ||
       parsed.options.json || parsed.options.json_stream ||
       parsed.options.resume_latest || parsed.options.resume_pick ||
       !parsed.options.prompt.empty() || !parsed.options.attach_paths.empty() ||
       !parsed.options.overrides.empty())) {
    parsed.error = "--control is a standalone management command";
  } else if (parsed.options.json && parsed.options.json_stream) {
    parsed.error = "--json and --json-stream are mutually exclusive";
  } else if ((parsed.options.json || parsed.options.json_stream) &&
             parsed.options.prompt.empty()) {
    parsed.error = "JSON output requires -p PROMPT";
  }
  return parsed;
}

std::span<const FlagSpec> FlagRegistry() { return kFlags; }

const char* UsageText() {
  static const std::string kText = [] {
    auto invocation_of = [](const FlagSpec& spec) {
      std::string invocation(spec.flag);
      if (!spec.value) return invocation;
      return spec.optional_value ? invocation + "[=" + spec.value + "]"
                                 : invocation + " " + spec.value;
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
  return kText.c_str();
}

}  // namespace uagent
