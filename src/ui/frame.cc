// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

#include "include/core/term.h"
#include "include/ui/interactive.h"

namespace uagent {

namespace {

constexpr std::string_view kFrameMagic{"\x1eUAGENT\x1f", 8};
constexpr size_t kFrameHeaderBytes = kFrameMagic.size() + 1 + sizeof(uint64_t);
constexpr uint8_t kRecordFrame = 1;
constexpr uint8_t kTailFrame = 2;

bool ValidFrameKind(uint8_t kind) {
  return kind == kRecordFrame || kind == kTailFrame;
}

uint64_t FrameLength(std::string_view header) {
  uint64_t length = 0;
  size_t begin = kFrameMagic.size() + 1;
  for (size_t index = begin; index < kFrameHeaderBytes; ++index) {
    length = (length << 8) | static_cast<unsigned char>(header[index]);
  }
  return length;
}

size_t PartialMagicSuffix(std::string_view text) {
  size_t limit = std::min(text.size(), kFrameMagic.size() - 1);
  for (size_t length = limit; length > 0; --length) {
    if (text.substr(text.size() - length) == kFrameMagic.substr(0, length)) {
      return length;
    }
  }
  return 0;
}

void WriteFrame(uint8_t kind, std::string_view payload) noexcept {
  if (!PersistentComposer()) {
    if (!payload.empty()) fwrite(payload.data(), 1, payload.size(), stdout);
    return;
  }

  std::array<unsigned char, kFrameHeaderBytes> header{};
  std::copy(kFrameMagic.begin(), kFrameMagic.end(), header.begin());
  header[kFrameMagic.size()] = kind;
  uint64_t length = static_cast<uint64_t>(payload.size());
  for (size_t index = 0; index < sizeof length; ++index) {
    header[kFrameHeaderBytes - index - 1] =
        static_cast<unsigned char>(length & 0xff);
    length >>= 8;
  }

  flockfile(stdout);
  fwrite(header.data(), 1, header.size(), stdout);
  if (!payload.empty()) fwrite(payload.data(), 1, payload.size(), stdout);
  funlockfile(stdout);
}

}  // namespace

void WriteTerminalRecord(std::string_view text) noexcept {
  WriteFrame(kRecordFrame, text);
  fflush(stdout);
}

void WriteTerminalTail(std::string_view text) noexcept {
  if (!text.empty()) WriteFrame(kTailFrame, text);
}

void InteractiveTranscript::AppendTail(std::string_view text,
                                       InteractiveOutputUpdate& update) {
  if (text.empty()) return;
  tail_.append(text);
  size_t split = tail_.rfind('\n');
  if (split != std::string::npos) {
    ++split;
    update.committed.append(tail_, 0, split);
    tail_.erase(0, split);
  }
  update.changed = true;
}

void InteractiveTranscript::CommitTail(InteractiveOutputUpdate& update) {
  if (tail_.empty()) return;
  update.committed += tail_;
  update.committed += '\n';
  tail_.clear();
  update.changed = true;
}

void InteractiveTranscript::ApplyFrame(uint8_t kind, std::string_view payload,
                                       InteractiveOutputUpdate& update) {
  if (kind == kTailFrame) {
    AppendTail(payload, update);
    return;
  }
  CommitTail(update);
  if (payload.empty()) return;
  update.committed += payload;
  if (payload.back() != '\n') update.committed += '\n';
  update.changed = true;
}

InteractiveOutputUpdate InteractiveTranscript::Feed(std::string_view bytes,
                                                    bool finish) {
  wire_.append(bytes);
  InteractiveOutputUpdate update;
  size_t offset = 0;
  while (offset < wire_.size()) {
    size_t marker = wire_.find(kFrameMagic.data(), offset, kFrameMagic.size());
    if (marker == std::string::npos) {
      std::string_view remainder(wire_.data() + offset, wire_.size() - offset);
      size_t keep = PartialMagicSuffix(remainder);
      AppendTail(remainder.substr(0, remainder.size() - keep), update);
      offset = wire_.size() - keep;
      break;
    }
    if (marker > offset) {
      AppendTail(std::string_view(wire_).substr(offset, marker - offset),
                 update);
      offset = marker;
    }
    if (wire_.size() - offset < kFrameHeaderBytes) break;

    std::string_view header(wire_.data() + offset, kFrameHeaderBytes);
    uint8_t kind = static_cast<uint8_t>(header[kFrameMagic.size()]);
    uint64_t encoded_length = FrameLength(header);
    if (!ValidFrameKind(kind) ||
        encoded_length > std::numeric_limits<size_t>::max()) {
      AppendTail(std::string_view(wire_).substr(offset, 1), update);
      ++offset;
      continue;
    }
    size_t length = static_cast<size_t>(encoded_length);
    size_t available = wire_.size() - offset - kFrameHeaderBytes;
    if (length > available) break;
    ApplyFrame(
        kind,
        std::string_view(wire_).substr(offset + kFrameHeaderBytes, length),
        update);
    offset += kFrameHeaderBytes + length;
  }
  wire_.erase(0, offset);

  if (finish && !wire_.empty()) {
    bool starts_frame = wire_.starts_with(kFrameMagic);
    if (starts_frame && wire_.size() >= kFrameHeaderBytes) {
      std::string_view header(wire_.data(), kFrameHeaderBytes);
      uint8_t kind = static_cast<uint8_t>(header[kFrameMagic.size()]);
      if (ValidFrameKind(kind)) {
        ApplyFrame(kind, std::string_view(wire_).substr(kFrameHeaderBytes),
                   update);
      }
    } else if (!starts_frame && !kFrameMagic.starts_with(wire_)) {
      AppendTail(wire_, update);
    }
    wire_.clear();
  }
  if (finish) CommitTail(update);
  update.tail = tail_;
  return update;
}

}  // namespace uagent
