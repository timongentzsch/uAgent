// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "include/ui/input_decoder.h"

// The composer's byte decoder reads whatever the terminal sends: escape
// sequences split across reads, terminal replies, unterminated payloads. Two
// invariants have to hold for arbitrary input -- the pending deque stays
// bounded, and feeding the same bytes in fragments decodes identically.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view input(reinterpret_cast<const char*>(data), size);

  uagent::TerminalInputDecoder whole;
  whole.Feed(input);
  size_t whole_tokens = 0;
  while (whole.Next(/*expire_escape=*/true)) ++whole_tokens;

  uagent::TerminalInputDecoder chunked;
  size_t chunked_tokens = 0;
  for (size_t offset = 0; offset < input.size();) {
    size_t width = 1 + static_cast<size_t>(data[offset]) % 7;
    width = std::min(width, input.size() - offset);
    chunked.Feed(input.substr(offset, width));
    offset += width;
    // Only complete tokens may be taken mid-stream: expiring the escape here
    // would resolve an ambiguity the whole-input decoder never sees.
    while (chunked.HasReady()) {
      if (!chunked.Next()) break;
      ++chunked_tokens;
    }
  }
  while (chunked.Next(/*expire_escape=*/true)) ++chunked_tokens;

  if (whole_tokens != chunked_tokens) __builtin_trap();
  return 0;
}
