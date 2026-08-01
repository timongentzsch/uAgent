// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_HEADLESS_H_
#define UAGENT_INCLUDE_APP_HEADLESS_H_

#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/core/usage.h"

namespace uagent {

inline json HeadlessResult(std::string answer, std::string error, json trace,
                           const Usage& usage, json routes, int exit_code) {
  return {{"schema", "uagent.headless.v1"},
          {"answer", std::move(answer)},
          {"error", error.empty() ? json(nullptr) : json(std::move(error))},
          {"trace", std::move(trace)},
          {"usage", UsageJson(usage)},
          {"routes", std::move(routes)},
          {"exit_code", exit_code}};
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_HEADLESS_H_
