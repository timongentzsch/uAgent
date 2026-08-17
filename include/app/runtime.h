// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_RUNTIME_H_
#define UAGENT_INCLUDE_APP_RUNTIME_H_

#include "include/agent/adaptive_system.h"
#include "include/api.h"
#include "include/core/env.h"
#include "include/core/usage.h"
#include "include/mcp/server.h"
#include "include/tools/process.h"

namespace uagent {

class CurlRuntime {
 public:
  CurlRuntime();
  ~CurlRuntime();

  CurlRuntime(const CurlRuntime&) = delete;
  CurlRuntime& operator=(const CurlRuntime&) = delete;

  bool Ready() const { return ready_; }

 private:
  bool ready_;
};

struct AppRuntime {
  explicit AppRuntime(RuntimeConfig parsed);
  ~AppRuntime();

  void Shutdown();

  RuntimeConfig config;
  Api api;
  AdaptiveSystemState adaptive_system;
  ProcessSupervisor processes;
  UsageAccumulator side_usage;
  McpRuntime mcp;

 private:
  bool shutdown_ = false;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_RUNTIME_H_
