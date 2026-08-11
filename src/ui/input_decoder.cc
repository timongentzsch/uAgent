// Copyright 2026 Timon Gentzsch

#include "include/ui/input_decoder.h"

#include <algorithm>
#include <string>
#include <utility>

#include "include/core/strings.h"

namespace uagent {
namespace {

constexpr std::string_view kPasteStart = "\x1b[200~";
constexpr std::string_view kPasteEnd = "\x1b[201~";

}  // namespace

bool ShouldRememberInput(std::string_view input) {
  return input.size() <= kInputHistoryEntryBytes &&
         input.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

void TerminalInputDecoder::Feed(const unsigned char* data, size_t size) {
  pending_.insert(pending_.end(), data, data + size);
}

void TerminalInputDecoder::Feed(std::string_view data) {
  Feed(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

bool TerminalInputDecoder::StartsWith(std::string_view sequence) const {
  if (pending_.size() < sequence.size()) return false;
  for (size_t index = 0; index < sequence.size(); ++index) {
    if (pending_[index] != static_cast<unsigned char>(sequence[index])) {
      return false;
    }
  }
  return true;
}

bool TerminalInputDecoder::IsPrefixOf(std::string_view sequence) const {
  if (pending_.size() > sequence.size()) return false;
  for (size_t index = 0; index < pending_.size(); ++index) {
    if (pending_[index] != static_cast<unsigned char>(sequence[index])) {
      return false;
    }
  }
  return true;
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

void TerminalInputDecoder::Consume(size_t count) {
  while (count-- > 0) pending_.pop_front();
}

void TerminalInputDecoder::ResetEscape() { escape_pending_ = false; }

bool TerminalInputDecoder::HasReady() const {
  if (pending_.empty()) return false;
  if (pasting_) return StartsWith(kPasteEnd) || !IsPrefixOf(kPasteEnd);
  if (pending_.front() != 0x1b) return true;
  if (pending_.size() == 1) {
    return !escape_pending_ ||
           std::chrono::steady_clock::now() - escape_started_ >=
               kInputEscapeDelay;
  }
  if (pending_[1] == '[') {
    return CompleteCsiBytes() > 0 || pending_.size() >= kInputSequenceBytes;
  }
  if (pending_[1] == 'O') return pending_.size() >= 3;
  return true;
}

std::optional<TerminalInputToken> TerminalInputDecoder::Next(
    bool expire_escape) {
  while (!pending_.empty()) {
    if (pasting_) {
      if (StartsWith(kPasteEnd)) {
        Consume(kPasteEnd.size());
        pasting_ = false;
        ReplaceAll(paste_, "\r\n", "\n");
        ReplaceAll(paste_, "\r", "\n");
        std::erase(paste_, '\0');
        TerminalInputToken token{TerminalInputTokenKind::kPaste,
                                 std::move(paste_), paste_overflow_};
        paste_.clear();
        paste_overflow_ = false;
        return token;
      }
      if (IsPrefixOf(kPasteEnd)) return std::nullopt;
      if (paste_.size() < kInputPasteBytes) {
        paste_ += static_cast<char>(pending_.front());
      } else {
        paste_overflow_ = true;
      }
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
      Consume(kPasteStart.size());
      ResetEscape();
      pasting_ = true;
      paste_.clear();
      paste_overflow_ = false;
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

    // A Meta pair must arrive inside the ambiguity window. CSI/SS3 sequences
    // stay exempt so arrows split by a slow terminal connection remain intact.
    if (pending_[1] == 0x1b ||
        (escape_was_pending && pending_[1] != '[' && pending_[1] != 'O' &&
         std::chrono::steady_clock::now() - escape_started_ >=
             kInputEscapeDelay)) {
      pending_.pop_front();
      ResetEscape();
      return TerminalInputToken{TerminalInputTokenKind::kEscape, "", false};
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
    std::string sequence;
    sequence.reserve(sequence_bytes);
    for (size_t index = 0; index < sequence_bytes; ++index) {
      sequence += static_cast<char>(pending_[index]);
    }
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
  paste_overflow_ = false;
  ResetEscape();
}

}  // namespace uagent
