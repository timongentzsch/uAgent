// Copyright 2026 Timon Gentzsch

#include "include/web/rfb_filter.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace uagent::web {
namespace {
constexpr size_t kMaxMessageBytes = size_t{256} * 1024;

uint16_t Read16(const std::string& input, size_t offset) {
  return static_cast<uint16_t>(
      (static_cast<uint16_t>(static_cast<unsigned char>(input[offset])) << 8U) |
      static_cast<unsigned char>(input[offset + 1]));
}

uint32_t Read32(const std::string& input, size_t offset) {
  uint32_t value = 0;
  for (size_t index = 0; index < 4; ++index) {
    value = (value << 8U) | static_cast<unsigned char>(input[offset + index]);
  }
  return value;
}

void Write16(std::string& output, uint16_t value) {
  output.push_back(static_cast<char>(value >> 8U));
  output.push_back(static_cast<char>(value & 0xffU));
}
}  // namespace

bool RfbInputFilter::Push(std::string_view data, std::string& output,
                          bool input) {
  output.clear();
  if (handshake_remaining_ > 0) {
    const size_t bytes = std::min(handshake_remaining_, data.size());
    output.append(data.begin(), data.begin() + bytes);
    data.remove_prefix(bytes);
    handshake_remaining_ -= bytes;
  }
  if (data.size() > kMaxMessageBytes - pending_.size()) return false;
  pending_.append(data.begin(), data.end());
  while (!pending_.empty()) {
    const auto type = static_cast<unsigned char>(pending_[0]);
    size_t size = 0;
    bool display = false;
    switch (type) {
      case 0:  // SetPixelFormat
        size = 20;
        display = true;
        break;
      case 2:  // SetEncodings
        if (pending_.size() < 4) return true;
        size = 4 + static_cast<size_t>(Read16(pending_, 2)) * 4;
        display = true;
        break;
      case 3:  // FramebufferUpdateRequest
        size = 10;
        display = true;
        break;
      case 4:  // KeyEvent
        size = 8;
        break;
      case 5:  // PointerEvent, with optional extended button byte
        if (pending_.size() < 2) return true;
        size = (static_cast<unsigned char>(pending_[1]) & 0x80U) ? 7 : 6;
        break;
      case 6: {  // ClientCutText, including the signed extended form
        if (pending_.size() < 8) return true;
        const uint32_t encoded = Read32(pending_, 4);
        const uint64_t payload = (encoded & 0x80000000U)
                                     ? uint64_t{0x100000000ULL} - encoded
                                     : encoded;
        if (payload > kMaxMessageBytes - 8) return false;
        size = 8 + static_cast<size_t>(payload);
        break;
      }
      case 150:  // EnableContinuousUpdates
        size = 10;
        display = true;
        break;
      case 248:  // ClientFence
        if (pending_.size() < 9) return true;
        size = 9 + static_cast<unsigned char>(pending_[8]);
        display = true;
        break;
      case 250:  // XVP power control
        size = 4;
        break;
      case 251:  // SetDesktopSize
        size = 24;
        break;
      case 255:  // QEMU extended key event
        size = 12;
        break;
      default:
        return false;
    }
    if (size > kMaxMessageBytes) return false;
    if (pending_.size() < size) return true;
    if (display || input) {
      output.append(pending_, 0, size);
      // Track what forwarded input holds down, so Release can lift it.
      if (type == 4 || type == 255) {
        std::string up = pending_.substr(0, size);
        const bool down = type == 4 ? up[1] != 0 : Read16(up, 2) != 0;
        if (type == 4) {
          up[1] = 0;
        } else {
          up[2] = up[3] = 0;
        }
        if (down) {
          held_keys_.insert(std::move(up));
        } else {
          held_keys_.erase(up);
        }
      } else if (type == 5) {
        buttons_ = static_cast<unsigned char>(pending_[1]) & 0x7fU;
        x_ = Read16(pending_, 2);
        y_ = Read16(pending_, 4);
      }
    }
    pending_.erase(0, size);
  }
  return true;
}

std::string RfbInputFilter::Release() {
  std::string output;
  for (const std::string& up : held_keys_) output += up;
  held_keys_.clear();
  if (buttons_) {
    output += std::string("\x05\0", 2);
    Write16(output, x_);
    Write16(output, y_);
    buttons_ = 0;
  }
  return output;
}

}  // namespace uagent::web
