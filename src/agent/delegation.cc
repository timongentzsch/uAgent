// Copyright 2026 Timon Gentzsch

#include "include/agent/delegation.h"

#include <string>

#include "include/api.h"
#include "include/core/env.h"
#include "include/core/strings.h"
#include "include/providers.h"

namespace uagent {

std::string DefaultSubagentModel(const Api& api) {
  std::string selection = NormalizeModelId(SubagentModel());
  if (!selection.empty()) return selection;
  return api.model;
}

std::string DelegationRuntimeContext(const Api& api) {
  // No provider list reaches here; the built-in templates still scope the
  // common routes, and a custom endpoint degrades to a bare model id.
  std::string parent = TerminalSafe(RouteSelection(api, {}));
  std::string child_model = DefaultSubagentModel(api);
  if (child_model == api.model) {
    return "[delegation: parent=" + parent + "; default=parent]";
  }
  return "[delegation: parent=" + parent +
         "; default=" + TerminalSafe(child_model) + "]";
}

}  // namespace uagent
