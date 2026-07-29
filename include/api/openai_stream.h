// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_OPENAI_STREAM_H_
#define UAGENT_INCLUDE_API_OPENAI_STREAM_H_

#include <map>
#include <string>
#include <string_view>

#include "include/api/types.h"

namespace uagent {

struct OpenAiStreamDelta {
  std::string content;
  std::string reasoning;
  bool activity = false;
};

OpenAiStreamDelta DecodeOpenAiStreamEvent(std::string_view data,
                                          ChatResult& result,
                                          std::map<int, ToolCall>& tool_calls);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_OPENAI_STREAM_H_
