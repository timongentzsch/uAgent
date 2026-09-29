// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TOOLS_ASK_H_
#define UAGENT_INCLUDE_TOOLS_ASK_H_
#include <functional>
#include <optional>
#include <string>

#include "include/core/json.h"
#include "include/tools/tool.h"
namespace uagent {
// Puts the questions to the person and returns their answers, one per
// question: {choices, other, image?}. Sets *eof when no answer came.
using AskPerson = std::function<std::string(const json& questions, bool* eof)>;

// Why `questions` is not a valid ask: 1 to 4 questions, each with a
// question, a header of at most 12 characters and 2 to 4 labelled options.
std::optional<std::string> AskQuestionsIssue(const json& questions);

Tool AskTool(AskPerson ask);
}  // namespace uagent
#endif
