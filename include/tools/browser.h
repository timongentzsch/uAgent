// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TOOLS_BROWSER_H_
#define UAGENT_INCLUDE_TOOLS_BROWSER_H_
// The browser tool: the model's page operations, run through the browser
// service. Each client supplies how a person is asked to finish a step.

#include <functional>
#include <string>

#include "include/tools/tool.h"
namespace uagent {
// Asks the person to finish a step in the browser; *eof when no one can answer.
using BrowserAsk = std::function<std::string(
    const std::string& interaction, const std::string& prompt, bool* eof)>;
Tool BrowserTool(std::string session_id, BrowserAsk ask);

// What a conversation last saw of a page, so the next result can be a diff.
class PageMemory {
 public:
  // `current` in full, or its diff to the text last shown for `url` if shorter.
  std::string Show(const std::string& url, const std::string& current,
                   bool diff = true);
  // For when the page changed by other means: another tab, an error, a dialog.
  void Forget() { url_.clear(); }

 private:
  std::string url_, text_;
};
}  // namespace uagent
#endif
