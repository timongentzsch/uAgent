// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_API_EXCHANGE_H_
#define UAGENT_INCLUDE_API_EXCHANGE_H_

#include <string>
#include <string_view>

#include "include/core/fd.h"
#include "include/core/json.h"

namespace uagent {
// Private HTTP bodies stay on disk. Only bounded metadata enters app events.
class HttpExchange {
 public:
  HttpExchange(bool enabled, std::string_view request, json metadata);
  void Headers(std::string_view bytes, bool outgoing);
  void Body(std::string_view bytes);
  json Finish(int64_t status, bool interrupted, const std::string& error);
  bool Enabled() const { return static_cast<bool>(response_); }

 private:
  void Publish();
  Fd response_;
  json metadata_;
  size_t bytes_ = 0;
};
json ContextPreview(const json& body);
}  // namespace uagent
#endif
