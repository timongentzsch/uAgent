// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_OPTIONS_H_
#define UAGENT_INCLUDE_APP_OPTIONS_H_

#include <string>
#include <vector>

namespace uagent {

enum class OptionsAction {
  kRun,
  kHelp,
  kVersion,
};

struct Options {
  bool yolo = false;
  bool trust_project = false;
  bool debug = false;
  bool resume_latest = false;
  bool resume_pick = false;
  std::string debug_path;
  std::string prompt;
  std::vector<std::string> attach_paths;
};

struct ParsedOptions {
  Options options;
  OptionsAction action = OptionsAction::kRun;
  std::string error;

  bool Ok() const { return error.empty(); }
};

ParsedOptions ParseOptions(int argc, char* const argv[]);
const char* UsageText();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_OPTIONS_H_
