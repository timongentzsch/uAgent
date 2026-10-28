// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TOOLS_BROWSER_H_
#define UAGENT_INCLUDE_TOOLS_BROWSER_H_
// The browser tool: the model's page operations, run through the browser
// service. Each client supplies how a person is asked to finish a step.

#include <functional>
#include <string>

#include "include/tools/tool.h"
namespace uagent {
// Asks the person to finish in the browser (interaction id, prompt) and
// returns their answer; sets *eof when no one can answer.
using BrowserAsk = std::function<std::string(
    const std::string& interaction, const std::string& prompt, bool* eof)>;
Tool BrowserTool(std::string session_id, BrowserAsk ask);
}  // namespace uagent
#endif
