// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_BROWSER_BROKER_H_
#define UAGENT_INCLUDE_BROWSER_BROKER_H_
// Admission and fairness for the browser service: who may use which
// instance and for how long.

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "include/core/json.h"

namespace uagent::browser {
// Execute is safe for concurrent callers: waiting is a short acquire/poll,
// never a held runtime lock.
class Broker {
 public:
  using Now = std::function<std::chrono::steady_clock::time_point()>;
  explicit Broker(std::string directory,
                  Now now = std::chrono::steady_clock::now);
  ~Broker();
  json Execute(const json& command);
  void Maintain();
  void Shutdown();

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace uagent::browser
#endif
