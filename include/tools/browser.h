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

// What a conversation was last shown of a page, so that the next result for
// the same page can carry only what changed: a zero-context line diff of the
// two texts, when that is shorter than the text itself.
class PageMemory {
 public:
  // The text to show for `current`, the result for the page at `url`. The
  // full text unless the previous result was for the same page and the diff
  // is shorter; with `diff` false (a read) always the full text. Either way
  // `current` becomes what the conversation has seen.
  std::string Show(const std::string& url, const std::string& current,
                   bool diff = true);
  // The page is no longer the one seen: a navigation by other means, another
  // tab, an error or a dialog.
  void Forget() { url_.clear(); }

 private:
  std::string url_, text_;
};
}  // namespace uagent
#endif
