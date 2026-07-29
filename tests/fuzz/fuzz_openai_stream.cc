// Copyright 2026 Timon Gentzsch

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

#include "include/api/openai_stream.h"
#include "include/api/types.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view input(reinterpret_cast<const char*>(data), size);
  uagent::ChatResult result;
  std::map<int, uagent::ToolCall> calls;
  uagent::DecodeOpenAiStreamEvent(input, result, calls);
  return 0;
}
