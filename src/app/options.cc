// Copyright 2026 Timon Gentzsch

#include "include/app/options.h"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace uagent {

ParsedOptions ParseOptions(int argc, char* const argv[]) {
  ParsedOptions parsed;
  for (int index = 1; index < argc; ++index) {
    std::string argument = argv[index];
    if (argument == "--yolo") {
      parsed.options.yolo = true;
    } else if (argument == "--json") {
      parsed.options.json = true;
    } else if (argument == "--json-stream") {
      parsed.options.json_stream = true;
    } else if (argument == "--budget") {
      if (++index >= argc) {
        parsed.error = "--budget requires a value";
        return parsed;
      }
      char* end = nullptr;
      errno = 0;
      parsed.options.budget = strtod(argv[index], &end);
      if (errno || !end || *end != '\0' || parsed.options.budget <= 0) {
        parsed.error = "--budget must be a positive dollar amount";
        return parsed;
      }
    } else if (argument == "-p" || argument == "--attach") {
      if (++index >= argc) {
        parsed.error = argument + " requires a value";
        return parsed;
      }
      if (argument == "-p") {
        parsed.options.prompt = argv[index];
      } else {
        parsed.options.attach_paths.push_back(argv[index]);
      }
    } else if (argument == "--continue" || argument == "-c") {
      parsed.options.resume_latest = true;
    } else if (argument == "--resume") {
      parsed.options.resume_pick = true;
    } else if (argument == "--trust-project-config") {
      parsed.options.trust_project = true;
    } else if (argument == "--debug") {
      parsed.options.debug = true;
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
         "[--trust-project-config] "
         "[--debug[=PATH]] "
         "[-p PROMPT] [--attach PATH] [-c] [--resume]\n\n"
         "  -p PROMPT   run one turn, print only the final answer, exit\n"
         "  --json      emit a stable JSON envelope in headless mode\n"
         "  --json-stream  emit versioned JSONL events in headless mode\n"
         "  --budget USD  cap total session spend between model calls\n"
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
