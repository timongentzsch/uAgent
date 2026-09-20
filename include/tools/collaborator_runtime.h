// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_COLLABORATOR_RUNTIME_H_
#define UAGENT_INCLUDE_TOOLS_COLLABORATOR_RUNTIME_H_

#include <memory>
#include <string>

#include "include/app/options.h"
#include "include/core/json.h"
#include "include/core/usage.h"
#include "include/tools/tool.h"

namespace uagent {

struct CollaboratorLaunch {
  std::string id, path, cwd, title, model, route;
  Options options;
  double remaining_cost = 0;
  int64_t remaining_tokens = 0;
};

// One retained child runtime owned by one parent conversation. Model work still
// runs through the ordinary application/session event chain; this object only
// owns its lifetime and correlates handoff checkpoints.
class CollaboratorRuntime {
 public:
  explicit CollaboratorRuntime(UsageAccumulator& usage);
  ~CollaboratorRuntime();
  CollaboratorRuntime(const CollaboratorRuntime&) = delete;
  CollaboratorRuntime& operator=(const CollaboratorRuntime&) = delete;

  ToolResult Handoff(CollaboratorLaunch launch, const std::string& prompt,
                     const ToolContext& context, bool* submitted = nullptr);
  ToolResult Message(const std::string& id, const std::string& text);
  ToolResult Interrupt(const std::string& id);
  ToolResult Stop(const std::string& id);
  json Snapshot(const std::string& id = "") const;
  json LiveView(const std::string& id) const;
  bool Active(const std::string& id) const;
  size_t Count() const;
  void Shutdown();

 private:
  struct State;
  ToolResult Control(const std::string& id, const std::string& kind,
                     const std::string& text = "");
  std::unique_ptr<State> state_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_COLLABORATOR_RUNTIME_H_
