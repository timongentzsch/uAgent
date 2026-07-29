// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "include/transport/sse.h"

namespace {

bool SameEvents(const std::vector<uagent::SseEvent>& left,
                const std::vector<uagent::SseEvent>& right) {
  if (left.size() != right.size()) return false;
  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index].event != right[index].event ||
        left[index].data != right[index].data ||
        left[index].id != right[index].id) {
      return false;
    }
  }
  return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view input(reinterpret_cast<const char*>(data), size);
  uagent::SseParser whole(64 * 1024);
  bool whole_ok = whole.Feed(input) && whole.Finish();
  std::vector<uagent::SseEvent> whole_events = whole.TakeEvents();

  uagent::SseParser chunked(64 * 1024);
  bool chunked_ok = true;
  for (size_t offset = 0; offset < input.size() && chunked_ok;) {
    size_t width = 1 + static_cast<size_t>(data[offset]) % 31;
    width = std::min(width, input.size() - offset);
    chunked_ok = chunked.Feed(input.substr(offset, width));
    offset += width;
  }
  chunked_ok = chunked_ok && chunked.Finish();
  std::vector<uagent::SseEvent> chunked_events = chunked.TakeEvents();
  if (whole_ok != chunked_ok || whole.Error() != chunked.Error() ||
      !SameEvents(whole_events, chunked_events)) {
    __builtin_trap();
  }
  return 0;
}
