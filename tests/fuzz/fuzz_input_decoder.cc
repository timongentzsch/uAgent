// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/ui/input_decoder.h"

namespace {

using uagent::TerminalInputToken;
using uagent::TerminalInputTokenKind;

// Where a read lands inside plain text decides only how many kText tokens
// carry it, never which bytes arrive or what surrounds them. Coalescing
// adjacent text is what lets the two decodings be compared by content: a
// weaker count-only invariant passes on a sequence decoded as text.
void Append(std::vector<std::pair<TerminalInputTokenKind, std::string>>& out,
            const TerminalInputToken& token) {
  if (token.kind == TerminalInputTokenKind::kText && !out.empty() &&
      out.back().first == TerminalInputTokenKind::kText) {
    out.back().second += token.text;
    return;
  }
  out.push_back({token.kind, token.text});
}

}  // namespace

// The composer's byte decoder reads whatever the terminal sends: escape
// sequences split across reads, terminal replies, unterminated payloads. Two
// invariants have to hold for arbitrary input -- the pending deque stays
// bounded, and feeding the same bytes in fragments decodes to the same tokens.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view input(reinterpret_cast<const char*>(data), size);

  std::vector<std::pair<TerminalInputTokenKind, std::string>> whole_tokens;
  uagent::TerminalInputDecoder whole;
  whole.Feed(input);
  while (auto token = whole.Next(/*expire_escape=*/true)) {
    Append(whole_tokens, *token);
  }

  std::vector<std::pair<TerminalInputTokenKind, std::string>> chunked_tokens;
  uagent::TerminalInputDecoder chunked;
  for (size_t offset = 0; offset < input.size();) {
    size_t width = 1 + static_cast<size_t>(data[offset]) % 7;
    width = std::min(width, input.size() - offset);
    chunked.Feed(input.substr(offset, width));
    offset += width;
    // Only complete tokens may be taken mid-stream: expiring the escape here
    // would resolve an ambiguity the whole-input decoder never sees.
    while (chunked.HasReady()) {
      auto token = chunked.Next();
      if (!token) break;
      Append(chunked_tokens, *token);
    }
  }
  while (auto token = chunked.Next(/*expire_escape=*/true)) {
    Append(chunked_tokens, *token);
  }

  if (whole_tokens != chunked_tokens) __builtin_trap();
  return 0;
}
