// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_OPENAI_STREAM_H_
#define UAGENT_INCLUDE_API_OPENAI_STREAM_H_

#include <map>
#include <string>
#include <string_view>

#include "include/api/wire.h"

namespace uagent {

void MergeStreamIdentity(std::string& target, const std::string& fragment);
void AddStreamAnnotation(const json& annotation, ChatResult& result);
WireStreamDelta DecodeOpenAiStreamEvent(std::string_view data,
                                        ChatResult& result,
                                        std::map<int, ToolCall>& tool_calls);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_OPENAI_STREAM_H_
