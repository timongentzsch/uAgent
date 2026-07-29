// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TRANSPORT_SSE_H_
#define UAGENT_INCLUDE_TRANSPORT_SSE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uagent {

struct SseEvent {
  std::string event;
  std::string data;
  std::string id;
};

class SseParser {
 public:
  explicit SseParser(size_t max_event_bytes = 1024 * 1024)
      : max_event_bytes_(max_event_bytes) {}

  bool Feed(std::string_view bytes);
  bool Finish();
  std::vector<SseEvent> TakeEvents();

  const std::string& Error() const { return error_; }

 private:
  bool ProcessLine();
  bool Dispatch();
  bool Append(std::string& target, std::string_view value);
  void ResetEvent();

  static constexpr size_t kMaxPendingEvents = 4096;

  size_t max_event_bytes_;
  bool pending_cr_ = false;
  bool has_data_ = false;
  std::string line_;
  std::string event_;
  std::string data_;
  std::string last_event_id_;
  std::vector<SseEvent> events_;
  std::string error_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TRANSPORT_SSE_H_
