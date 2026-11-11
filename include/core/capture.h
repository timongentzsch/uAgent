// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_CAPTURE_H_
#define UAGENT_INCLUDE_CORE_CAPTURE_H_
// Running a short helper process and capturing its output.

#include <string>
#include <vector>
namespace uagent {
// Shared noninteractive SSH policy; a master owns the multiplexed transport.
std::vector<std::string> SshCommand(const std::string& control,
                                    const std::string& address,
                                    bool master = false);
struct CapturedProcess {
  int status = -1;
  std::string output, error;
  bool Ok() const { return status == 0 && error.empty(); }
};
// Short native helpers, with argv (no shell), bounded output and lifetime.
CapturedProcess CaptureProcess(const std::vector<std::string>& arguments,
                               int timeout_seconds = 10,
                               const std::string& input = "",
                               char* const* environment = nullptr);
}  // namespace uagent
#endif
