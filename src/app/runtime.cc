// Copyright 2026 Timon Gentzsch

#include "include/app/runtime.h"

#include <curl/curl.h>

#include <utility>

#include "include/tools/jobs.h"

namespace uagent {

CurlRuntime::CurlRuntime()
    : ready_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}

CurlRuntime::~CurlRuntime() {
  if (ready_) curl_global_cleanup();
}

AppRuntime::AppRuntime(RuntimeConfig parsed)
    : config(std::move(parsed)), api(config) {}

AppRuntime::~AppRuntime() { Shutdown(); }

void AppRuntime::Shutdown() {
  if (shutdown_) return;
  shutdown_ = true;
  mcp.ShutdownAll();
  BgShutdownAll(processes);
}

}  // namespace uagent
