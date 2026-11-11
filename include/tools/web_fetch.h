// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_WEB_FETCH_H_
#define UAGENT_INCLUDE_TOOLS_WEB_FETCH_H_
// The tool that reads a known URL, for when search only summarised the page.

#include <string>

#include "include/api/client.h"
#include "include/tools/tool.h"

namespace uagent {

// Markup to the text a reader would see, block boundaries as newlines.
std::string HtmlToText(const std::string& html);

// What a fetched page gives the model: its text, an attached PDF, or why not.
ToolResult ReadFetchedPage(const std::string& url, WebResponse page,
                           const std::string& call_id);

Tool WebFetchTool(Api& api);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_WEB_FETCH_H_
