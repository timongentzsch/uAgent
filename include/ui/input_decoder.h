// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_INPUT_DECODER_H_
#define UAGENT_INCLUDE_UI_INPUT_DECODER_H_
// Shared bounded terminal byte decoder for libedit and the persistent composer.

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace uagent {

inline constexpr size_t kInputHistoryEntries = 200;
inline constexpr size_t kInputHistoryEntryBytes = 16 * 1024;
inline constexpr size_t kInputBufferBytes = 64 * 1024;
inline constexpr size_t kInputPasteBytes = kInputBufferBytes;
inline constexpr size_t kInputSequenceBytes = 64;
inline constexpr std::chrono::milliseconds kInputEscapeDelay{50};

bool ShouldRememberInput(std::string_view input);

enum class TerminalInputTokenKind { kText, kSequence, kPaste, kEscape };

struct TerminalInputToken {
  TerminalInputTokenKind kind = TerminalInputTokenKind::kText;
  std::string text;
  bool overflow = false;
};

class TerminalInputDecoder {
 public:
  void Feed(const unsigned char* data, size_t size);
  void Feed(std::string_view data);
  bool HasReady() const;
  std::optional<TerminalInputToken> Next(bool expire_escape = false);
  void Reset();

 private:
  bool StartsWith(std::string_view sequence) const;
  bool IsPrefixOf(std::string_view sequence) const;
  size_t CompleteCsiBytes() const;
  void Consume(size_t count);
  void ResetEscape();

  std::deque<unsigned char> pending_;
  std::string paste_;
  bool pasting_ = false;
  bool paste_overflow_ = false;
  bool escape_pending_ = false;
  std::chrono::steady_clock::time_point escape_started_{};
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_INPUT_DECODER_H_
