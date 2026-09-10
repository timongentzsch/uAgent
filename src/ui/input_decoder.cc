// Copyright 2026 Timon Gentzsch

#include "include/ui/input_decoder.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr std::string_view kPasteStart = "\x1b[200~";
constexpr std::string_view kPasteEnd = "\x1b[201~";

}  // namespace

bool ShouldRememberInput(std::string_view input) {
  size_t first = input.find_first_not_of(" \t\r\n");
  // Direct configuration edits may contain credentials.
  return input.size() <= kInputHistoryEntryBytes &&
         first != std::string_view::npos &&
         !(input.substr(first).starts_with("/config") &&
           input.size() > first + 7 &&
           std::string_view(" \t\r\n").find(input[first + 7]) !=
               std::string_view::npos);
}

void TerminalInputDecoder::StartPaste() {
  Consume(kPasteStart.size());
  ResetEscape();
  pasting_ = true;
  paste_.clear();
  paste_overflow_ = false;
}

void TerminalInputDecoder::AppendPasteByte(unsigned char byte) {
  if (paste_.size() < kInputPasteBytes) {
    paste_.push_back(static_cast<char>(byte));
  } else {
    paste_overflow_ = true;
  }
}

void TerminalInputDecoder::FeedPaste(const unsigned char*& data, size_t& size) {
  while (pasting_) {
    if (StartsWith(kPasteEnd)) {
      Consume(kPasteEnd.size());
      pasting_ = false;
      paste_ready_ = true;
      return;
    }
    if (size == 0) return;

    pending_.push_back(*data);
    ++data;
    --size;
    while (!pending_.empty() && !IsPrefixOf(kPasteEnd)) {
      AppendPasteByte(pending_.front());
      pending_.pop_front();
    }
  }
}

TerminalInputToken TerminalInputDecoder::TakePaste() {
  ReplaceAll(paste_, "\r\n", "\n");
  ReplaceAll(paste_, "\r", "\n");
  std::erase(paste_, '\0');
  TerminalInputToken token{TerminalInputTokenKind::kPaste, std::move(paste_),
                           paste_overflow_};
  paste_.clear();
  paste_ready_ = false;
  paste_overflow_ = false;
  return token;
}

void TerminalInputDecoder::Feed(const unsigned char* data, size_t size) {
  if (size == 0 || pending_overflow_) return;

  while (size > 0) {
    if (pasting_) {
      FeedPaste(data, size);
      if (pasting_) return;
      continue;
    }

    if (!paste_ready_ && IsPrefixOf(kPasteStart)) {
      const size_t missing = kPasteStart.size() - pending_.size();
      const size_t count = std::min(size, missing);
      pending_.insert(pending_.end(), data, data + count);
      data += count;
      size -= count;
      if (StartsWith(kPasteStart)) {
        StartPaste();
        continue;
      }
      if (IsPrefixOf(kPasteStart) || size == 0) return;
    }

    const size_t capacity = kInputBufferBytes - pending_.size();
    if (size > capacity) {
      pending_.clear();
      ResetEscape();
      pending_overflow_ = true;
      return;
    }
    pending_.insert(pending_.end(), data, data + size);
    return;
  }
}

void TerminalInputDecoder::Feed(std::string_view data) {
  Feed(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

// Compare the first `count` pending bytes against `sequence`.
bool TerminalInputDecoder::MatchesFirst(std::string_view sequence,
                                        size_t count) const {
  return std::equal(pending_.begin(),
                    pending_.begin() + static_cast<std::ptrdiff_t>(count),
                    sequence.begin(), [](unsigned char pending, char wanted) {
                      return pending == static_cast<unsigned char>(wanted);
                    });
}

bool TerminalInputDecoder::StartsWith(std::string_view sequence) const {
  return pending_.size() >= sequence.size() &&
         MatchesFirst(sequence, sequence.size());
}

bool TerminalInputDecoder::IsPrefixOf(std::string_view sequence) const {
  return pending_.size() <= sequence.size() &&
         MatchesFirst(sequence, pending_.size());
}

size_t TerminalInputDecoder::CompleteCsiBytes() const {
  if (pending_.size() < 3 || pending_[0] != 0x1b || pending_[1] != '[') {
    return 0;
  }
  for (size_t index = 2; index < pending_.size(); ++index) {
    if (pending_[index] >= 0x40 && pending_[index] <= 0x7e) return index + 1;
  }
  return 0;
}

bool TerminalInputDecoder::StartsStringSequence() const {
  if (pending_.size() < 2 || pending_[0] != 0x1b) return false;
  const unsigned char introducer = pending_[1];
  return introducer == ']' || introducer == 'P' || introducer == 'X' ||
         introducer == '^' || introducer == '_';
}

size_t TerminalInputDecoder::CompleteStringSequenceBytes() const {
  for (size_t index = 2; index < pending_.size(); ++index) {
    if (pending_[index] == 0x07) return index + 1;  // BEL
    if (pending_[index] == 0x1b && index + 1 < pending_.size() &&
        pending_[index + 1] == '\\') {
      return index + 2;  // ST
    }
  }
  return 0;
}

bool TerminalInputDecoder::StartsX10Mouse() const {
  return pending_.size() >= 3 && pending_[0] == 0x1b && pending_[1] == '[' &&
         pending_[2] == 'M';
}

void TerminalInputDecoder::Consume(size_t count) {
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(count));
}

void TerminalInputDecoder::ResetEscape() { escape_pending_ = false; }

bool TerminalInputDecoder::HasReady() const {
  if (paste_ready_ || pending_overflow_) return true;
  if (pending_.empty()) return false;
  if (pasting_) return StartsWith(kPasteEnd) || !IsPrefixOf(kPasteEnd);
  if (pending_.front() != 0x1b) return true;
  if (pending_.size() == 1) {
    return !escape_pending_ ||
           std::chrono::steady_clock::now() - escape_started_ >=
               kInputEscapeDelay;
  }
  if (StartsStringSequence()) {
    return CompleteStringSequenceBytes() > 0 ||
           pending_.size() >= kInputStringSequenceBytes;
  }
  if (StartsX10Mouse()) return pending_.size() >= 6;
  if (pending_[1] == '[') {
    return CompleteCsiBytes() > 0 || pending_.size() >= kInputSequenceBytes;
  }
  if (pending_[1] == 'O') return pending_.size() >= 3;
  return true;
}

std::optional<std::chrono::steady_clock::time_point>
TerminalInputDecoder::WakeDeadline() const {
  if (!escape_pending_ || pending_.size() != 1 || pending_.front() != 0x1b) {
    return std::nullopt;
  }
  return escape_started_ + kInputEscapeDelay;
}

std::optional<TerminalInputToken> TerminalInputDecoder::Next(
    bool expire_escape) {
  if (paste_ready_) {
    pending_overflow_ = false;
    return TakePaste();
  }
  if (pending_overflow_) {
    pending_overflow_ = false;
    return std::nullopt;
  }

  while (!pending_.empty()) {
    if (pasting_) {
      if (StartsWith(kPasteEnd)) {
        Consume(kPasteEnd.size());
        pasting_ = false;
        return TakePaste();
      }
      if (IsPrefixOf(kPasteEnd)) return std::nullopt;
      AppendPasteByte(pending_.front());
      pending_.pop_front();
      continue;
    }

    if (pending_.front() != 0x1b) {
      char byte = static_cast<char>(pending_.front());
      pending_.pop_front();
      return TerminalInputToken{TerminalInputTokenKind::kText,
                                std::string(1, byte)};
    }

    bool escape_was_pending = escape_pending_;
    if (!escape_pending_) {
      escape_pending_ = true;
      escape_started_ = std::chrono::steady_clock::now();
    }
    if (StartsWith(kPasteStart)) {
      StartPaste();
      continue;
    }
    if (pending_.size() == 1) {
      if (!expire_escape && std::chrono::steady_clock::now() - escape_started_ <
                                kInputEscapeDelay) {
        return std::nullopt;
      }
      pending_.pop_front();
      ResetEscape();
      return TerminalInputToken{TerminalInputTokenKind::kEscape, "", false};
    }

    // A Meta pair must arrive inside the ambiguity window. CSI, SS3 and the
    // string introducers stay exempt: a sequence split by a slow connection
    // must not decay into a bare Escape plus its payload as typed text.
    const bool sequence_introducer =
        pending_[1] == '[' || pending_[1] == 'O' || StartsStringSequence();
    if (pending_[1] == 0x1b ||
        (escape_was_pending && !sequence_introducer &&
         std::chrono::steady_clock::now() - escape_started_ >=
             kInputEscapeDelay)) {
      pending_.pop_front();
      ResetEscape();
      return TerminalInputToken{TerminalInputTokenKind::kEscape, "", false};
    }

    if (StartsStringSequence()) {
      size_t string_bytes = CompleteStringSequenceBytes();
      if (string_bytes == 0) {
        if (pending_.size() < kInputStringSequenceBytes) return std::nullopt;
        // Unterminated past the bound: drop it rather than grow.
        Consume(pending_.size());
        ResetEscape();
        continue;
      }
      std::string sequence(
          pending_.begin(),
          pending_.begin() + static_cast<std::ptrdiff_t>(string_bytes));
      Consume(string_bytes);
      ResetEscape();
      return TerminalInputToken{TerminalInputTokenKind::kSequence,
                                std::move(sequence)};
    }

    // The coordinate bytes may be arbitrary, including 0x1b, so take a block.
    if (StartsX10Mouse()) {
      if (pending_.size() < 6) return std::nullopt;
      std::string sequence(pending_.begin(), pending_.begin() + 6);
      Consume(6);
      ResetEscape();
      return TerminalInputToken{TerminalInputTokenKind::kSequence,
                                std::move(sequence)};
    }

    size_t sequence_bytes = CompleteCsiBytes();
    if (pending_[1] == '[' && sequence_bytes == 0 &&
        pending_.size() < kInputSequenceBytes) {
      return std::nullopt;
    }
    if (pending_[1] == '[' && sequence_bytes == 0) {
      sequence_bytes = kInputSequenceBytes;
    }
    if (pending_[1] == 'O' && pending_.size() < 3) return std::nullopt;
    if (pending_[1] == 'O') sequence_bytes = 3;
    if (sequence_bytes == 0) {
      sequence_bytes = 2;
    }
    std::string sequence(
        pending_.begin(),
        pending_.begin() + static_cast<std::ptrdiff_t>(sequence_bytes));
    Consume(sequence_bytes);
    ResetEscape();
    return TerminalInputToken{TerminalInputTokenKind::kSequence,
                              std::move(sequence)};
  }
  return std::nullopt;
}

void TerminalInputDecoder::Reset() {
  pending_.clear();
  paste_.clear();
  pasting_ = false;
  paste_ready_ = false;
  paste_overflow_ = false;
  pending_overflow_ = false;
  ResetEscape();
}

}  // namespace uagent
