// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TURN_STATUS_H_
#define UAGENT_INCLUDE_CORE_TURN_STATUS_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "include/core/json.h"

namespace uagent {

enum class EventId : uint16_t;

// What a session with nothing running says.
inline constexpr const char* kIdleStatus = "Ready";

// Presentation policies only: these values never enter model history or
// authorize execution. Work is proportional to new text, with bounded storage.
std::string StatusLabel(std::string_view text);

class TurnStatus {
 public:
  bool Consume(EventId id, const json& data);
  const json& Status() const { return status_; }
  uint64_t Revision() const { return revision_; }

 private:
  struct Call {
    std::string label;
    bool running = false;
  };
  void Reasoning(const json& data);
  json Project() const;

  bool active_ = false;
  int64_t turn_ = 0;
  uint64_t revision_ = 0;
  std::string response_, phase_ = "idle";
  std::string part_, line_, excerpt_;
  bool long_line_ = false;
  std::map<std::string, Call> calls_;
  std::set<std::string> decisions_;
  // Tools the provider is running, by call: web_search, code_execution, ...
  std::map<std::string, std::string> hosted_;
  json retry_ = nullptr;
  json status_ = nullptr;
};

}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_TURN_STATUS_H_
