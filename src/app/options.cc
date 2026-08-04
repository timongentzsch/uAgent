// Copyright 2026 Timon Gentzsch

#include "include/app/options.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/strings.h"

namespace uagent {

ParsedOptions ParseOptions(int argc, char* const argv[]) {
  static constexpr std::pair<std::string_view, bool Options::*> kFlags[] = {
      {"--yolo", &Options::yolo},
      {"--json", &Options::json},
      {"--json-stream", &Options::json_stream},
      {"--no-memory", &Options::no_memory},
      {"--continue", &Options::resume_latest},
      {"-c", &Options::resume_latest},
      {"--resume", &Options::resume_pick},
      {"--trust-project-config", &Options::trust_project},
      {"--debug", &Options::debug},
  };
  ParsedOptions parsed;
  for (int index = 1; index < argc; ++index) {
    std::string argument = argv[index];
    auto flag = std::find_if(
        std::begin(kFlags), std::end(kFlags),
        [&](const auto& value) { return value.first == argument; });
    if (flag != std::end(kFlags)) {
      parsed.options.*flag->second = true;
    } else if (argument == "--budget" || argument == "-p" ||
               argument == "--attach" || argument == "--memory-consolidate") {
      if (++index >= argc) {
        parsed.error = argument + " requires a value";
        return parsed;
      }
      if (argument == "--budget" &&
          (!ParseFiniteDouble(argv[index], parsed.options.budget) ||
           parsed.options.budget <= 0)) {
        parsed.error = "--budget must be a positive dollar amount";
        return parsed;
      } else if (argument == "-p") {
        parsed.options.prompt = argv[index];
      } else if (argument == "--attach") {
        parsed.options.attach_paths.push_back(argv[index]);
      } else if (argument == "--memory-consolidate") {
        parsed.options.memory_source = argv[index];
      }
    } else if (argument.starts_with("--debug=")) {
      parsed.options.debug = true;
      parsed.options.debug_path = argument.substr(8);
    } else if (argument == "--version") {
      parsed.action = OptionsAction::kVersion;
      return parsed;
    } else if (argument == "-h" || argument == "--help") {
      parsed.action = OptionsAction::kHelp;
      return parsed;
    } else {
      parsed.error = "unknown flag: " + argument;
      return parsed;
    }
  }
  if (parsed.options.json && parsed.options.json_stream) {
    parsed.error = "--json and --json-stream are mutually exclusive";
  } else if ((parsed.options.json || parsed.options.json_stream) &&
             parsed.options.prompt.empty()) {
    parsed.error = "JSON output requires -p PROMPT";
  }
  return parsed;
}

const char* UsageText() {
  return "usage: uagent [--yolo] [--json|--json-stream] [--budget USD] "
         "[--no-memory] "
         "[--trust-project-config] "
         "[--debug[=PATH]] "
         "[-p PROMPT] [--attach PATH] [-c] [--resume]\n\n"
         "  -p PROMPT   run one turn, print only the final answer, exit\n"
         "  --json      emit a stable JSON envelope in headless mode\n"
         "  --json-stream  emit versioned JSONL events in headless mode\n"
         "  --budget USD  cap total session spend between model calls\n"
         "  --no-memory  disable memory recall and writes for this session\n"
         "  --attach PATH  send an image or document with the first message\n"
         "  -c          resume the most recent saved session\n"
         "  --resume    pick a saved session to resume at startup\n"
         "  --version   print the installed version\n"
         "  --trust-project-config  allow this workspace's .mcp.json and "
         ".uagent/.config\n\n"
         "config: ./.uagent/.config when trusted, then ~/.uagent/.config; "
         "process UAGENT_* variables override both\n";
}

}  // namespace uagent
