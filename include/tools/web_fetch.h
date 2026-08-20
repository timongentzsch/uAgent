// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_FETCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_FETCH_H_
// One model-facing tool for reading a known URL, for when search summarised
// a page instead of answering from it.

#include <string>

#include "include/api.h"
#include "include/tools/tool.h"

namespace uagent {

// Markup to the text a reader would see: script, style and comments dropped,
// block boundaries kept as newlines, entities decoded. Exposed for tests.
std::string HtmlToText(const std::string& html);

Tool WebFetchTool(Api& api);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_FETCH_H_
