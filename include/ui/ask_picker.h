// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_UI_ASK_PICKER_H_
#define UAGENT_INCLUDE_UI_ASK_PICKER_H_
#include <functional>
#include <string>

#include "include/core/json.h"

namespace uagent {
// Answers an ask tool's questions in a raw-mode terminal, one at a time:
// up and down move, space toggles an option of a multi-select question,
// enter chooses (on Other it asks for their own words), i attaches an image
// by path, escape cancels. Returns the reply fields {text, attachments}, or
// null when cancelled or when `live` says the decision was answered
// elsewhere. `write` draws; input is read from stdin.
json PickAskAnswers(const json& questions,
                    const std::function<void(const std::string&)>& write,
                    const std::function<bool()>& live);

// The same answers from one typed line, for a terminal without raw input:
// one part per question, split by ";", where option numbers ("1" or "1,3")
// choose and anything else is the person's own words.
std::string AskAnswersFromLine(const json& questions, const std::string& line);
}  // namespace uagent
#endif
