// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_RFB_FILTER_H_
#define UAGENT_INCLUDE_WEB_RFB_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace uagent::web {

// Parses a viewer's client-to-server RFB stream for its whole lifetime, so
// input can be allowed and blocked mid-stream without losing message framing.
class RfbInputFilter {
 public:
  // Appends the complete messages to forward. Display messages always pass;
  // keys, pointer and clipboard pass only while `input` is allowed. Returns
  // false on a malformed or unsupported message.
  bool Push(std::string_view data, std::string& output, bool input);
  // Messages that lift every key and button that forwarded input still holds.
  std::string Release();

 private:
  size_t handshake_remaining_ = 14;
  std::string pending_;
  // The key-up message for each key forwarded input holds down, in the form
  // it was pressed (plain or QEMU extended, which servers track apart).
  std::set<std::string> held_keys_;
  unsigned char buttons_ = 0;
  uint16_t x_ = 0, y_ = 0;
};

}  // namespace uagent::web
#endif
