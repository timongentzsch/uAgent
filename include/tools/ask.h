// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_TOOLS_ASK_H_
#define UAGENT_INCLUDE_TOOLS_ASK_H_
// The ask tool: validates a model's questions and puts them to the person
// through a callback each client supplies.

#include <functional>
#include <optional>
#include <string>

#include "include/core/json.h"
#include "include/tools/tool.h"
namespace uagent {
// One answer per question, {choices, other, image?}; *eof when none came.
using AskPerson = std::function<std::string(const json& questions, bool* eof)>;
// Stores an option's image in the session: {id, name, mime, bytes}, or `error`.
using AskImage =
    std::function<json(const std::string& path, std::string& error)>;

// Why `questions` is not a valid ask; nothing when it is.
std::optional<std::string> AskQuestionsIssue(const json& questions);

// Without `image`, option images reach clients by path only (a terminal).
Tool AskTool(AskPerson ask, AskImage image = {});
}  // namespace uagent
#endif
