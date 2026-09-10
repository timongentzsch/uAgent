// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_CAPTURE_H_
#define UAGENT_INCLUDE_CORE_CAPTURE_H_
#include <string>
#include <vector>
namespace uagent {
struct CapturedProcess {
  int status = -1;
  std::string output, error;
  bool Ok() const { return status == 0 && error.empty(); }
};
// Short native helpers, with argv (no shell), bounded output and lifetime.
CapturedProcess CaptureProcess(const std::vector<std::string>& arguments,
                               int timeout_seconds = 10,
                               const std::string& input = "");
}  // namespace uagent
#endif
