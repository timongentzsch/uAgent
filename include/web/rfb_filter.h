// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_RFB_FILTER_H_
#define UAGENT_INCLUDE_WEB_RFB_FILTER_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace uagent::web {

// Passes the RFB 3.8 handshake and display requests while removing keyboard,
// pointer, clipboard and desktop-control messages. The appliance fixes Xvnc
// to SecurityTypes=None, so its client handshake is always fourteen bytes.
class RfbViewOnlyFilter {
 public:
  bool Push(std::string_view input, std::string& output);

 private:
  size_t handshake_remaining_ = 14;
  std::string pending_;
};

}  // namespace uagent::web
#endif
