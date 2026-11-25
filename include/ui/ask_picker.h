// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_UI_ASK_PICKER_H_
#define UAGENT_INCLUDE_UI_ASK_PICKER_H_
// The terminal's answer to an ask tool: a raw-mode picker and a typed-line
// fallback.

#include <functional>
#include <optional>
#include <string>

#include "include/core/json.h"

namespace uagent {
class LiveRegion;

// Answers an ask tool's questions in a raw-mode terminal, drawn in `region`.
// Returns {text, attachments}, or null when cancelled or answered elsewhere
// (`live`). `wake` turns readable when `live` may have changed.
json PickAskAnswers(const json& questions, LiveRegion& region, int wake,
                    const std::function<bool()>& live);

// A password typed in a raw-mode terminal under `prompt`, shown as stars;
// nullopt when escaped or answered elsewhere (as PickAskAnswers).
std::optional<std::string> PickSecret(const std::string& prompt,
                                      LiveRegion& region, int wake,
                                      const std::function<bool()>& live);

// The same from one typed line, for a terminal without raw input: parts
// split by ";"; option numbers ("1" or "1,3") choose, the rest is free text.
std::string AskAnswersFromLine(const json& questions, const std::string& line);

// One line typed under `lead`; nullopt when escaped or closed.
using LineReader =
    std::function<std::optional<std::string>(const std::string& lead)>;

// An MCP server's elicitation answered a line at a time, a form field by
// field and then reviewed; the ElicitResult as text, nullopt when cancelled.
// Without `escape` (a cooked terminal) "c" cancels instead of Esc.
std::optional<std::string> AnswerElicitation(const json& shown,
                                             const LineReader& line,
                                             bool escape);

// The same in a raw-mode terminal, drawn in `region` (as PickAskAnswers).
std::optional<std::string> PickElicitation(const json& shown,
                                           LiveRegion& region, int wake,
                                           const std::function<bool()>& live);
}  // namespace uagent
#endif
