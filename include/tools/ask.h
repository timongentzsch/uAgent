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
// Snapshots an image the agent made for an option into the session, so every
// client can show it: the stored asset {id, name, mime, bytes}, or empty with
// `error` set.
using AskImage =
    std::function<json(const std::string& path, std::string& error)>;

// Why `questions` is not a valid ask: 1 to 4 questions, each with a
// question, a header of at most 12 characters and 2 to 4 labelled options;
// an option's image is an image file in the workspace and comes with a
// description, its alt text; a preview is at most 40 lines.
std::optional<std::string> AskQuestionsIssue(const json& questions);

// Without `image`, option images reach clients by path only (a terminal).
Tool AskTool(AskPerson ask, AskImage image = {});
}  // namespace uagent
#endif
