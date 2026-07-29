// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_RUNTIME_H_
#define UAGENT_INCLUDE_APP_RUNTIME_H_

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

  RuntimeConfig config;
  Api api;
  ProcessSupervisor processes;
  UsageAccumulator side_usage;
  SideTaskSupervisor side_tasks;
  McpRuntime mcp;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_RUNTIME_H_
