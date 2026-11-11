// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_EXECUTION_H_
#define UAGENT_INCLUDE_APP_EXECUTION_H_
#include <memory>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent::execution {
// SSH/control details are private main-side launch data, never provider config.
struct Target {
  std::string address, control, executable, client, runtime, cwd, auth;
};
Target TargetFromJson(const json& value);
class Client {
 public:
  explicit Client(Target target);
  ~Client();
  json Call(json request, const ToolContext* context = nullptr);
  bool Open(std::string& error);
  // Release the SSH listener when its owning runtime or terminal ends.
  void Release();
  // The gateway's tools, each with the presentation of its like in `tools`.
  std::vector<Tool> Tools(const std::string& session_path,
                          const std::vector<Tool>& tools);
  Target ChildTarget(const std::string& runtime) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void SetClient(std::shared_ptr<Client> client);
std::shared_ptr<Client> CurrentClient();
// Invoked before application/config startup. No agent, provider or web host.
int Main(int argc, char** argv);
json EncodeResult(const ToolResult& result);
ToolResult DecodeResult(const json& value);
}  // namespace uagent::execution
#endif
