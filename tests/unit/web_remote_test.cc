// Copyright 2026 Timon Gentzsch
#include <poll.h>
#include <sys/socket.h>

#include <string>
#include <thread>

#include "include/app/execution.h"
#include "include/app/launch.h"
#include "include/core/config_registry.h"
#include "include/core/fd.h"
#include "include/core/workspace.h"
#include "src/app/execution_internal.h"
#include "tests/unit/test_support.h"
namespace uagent {
TEST_CASE(TestWorkspaceIdentityKeepsLocalHistoryAndSeparatesHosts) {
  TestWorkspace workspace("workspace-identity");
  const std::string path = workspace.workspace.string();
  CHECK(WorkspaceId(path) == HashHex(path));
  CHECK(WorkspaceId(path, "first") != WorkspaceId(path));
  CHECK(WorkspaceId(path, "first") != WorkspaceId(path, "second"));
  CHECK(HistoryPath(path, "conversation", "first") !=
        HistoryPath(path, "conversation", "second"));
}
TEST_CASE(TestExecutionResultKeepsUndoAndActivityFacts) {
  ToolResult original = ToolSuccess("finished");
  std::string bytes;
  for (int i = 0; i < 256; ++i) bytes += static_cast<char>(i);
  original.effect = FileEffect{"/remote/file", true, bytes, "hash", 0600};
  original.background = 42;
  original.ended.push_back({43, 0, "child done", 123.0, false});
  original.facts.exit = ResultFacts::Exit{7, 0};
  original.parts = json::array({{{"kind", "file"}, {"name", "report"}}});
  const ToolResult copy = execution::DecodeResult(
      json::parse(JsonDump(execution::EncodeResult(original))));
  CHECK(copy.output == original.output);
  CHECK(copy.effect && copy.effect->before == original.effect->before);
  CHECK(copy.effect && copy.effect->before_mode == 0600);
  CHECK(copy.background == 42 && copy.ended.size() == 1);
  CHECK(copy.facts.exit && copy.facts.exit->code == 7);
  CHECK(copy.parts == original.parts);
  CHECK(execution::DecodeResult({{"error", "Disconnected"}}).error ==
        ToolErrorCode::kUnavailable);
}
// The gateway is opened with the sandbox preset in effect when the client is
// made. Bootstrap makes it after the settings, so an agent that may not edit
// (readonly, session_worker.cc) only reads there too, whoever started it.
TEST_CASE(TestExecutionClientOpensWithTheEffectivePreset) {
  TestWorkspace workspace("execution-preset");
  OverrideSetting("UAGENT_SANDBOX", "readonly");
  const std::string client = HashHex("client"), runtime = HashHex("runtime");
  REQUIRE(EnsurePrivateDirectory(RuntimeDir()));
  Fd listener = ListenUnix(
      RuntimeDir() + "/e-" + HashHex(client + runtime + "readonly") + ".sock",
      1);
  REQUIRE(listener.Get() >= 0);
  json request;
  std::thread gateway([&] {
    pollfd ready{listener.Get(), POLLIN, 0};
    if (poll(&ready, 1, 5000) <= 0) return;
    Fd peer(accept(listener.Get(), nullptr, nullptr));
    request = execution::Receive(
        peer.Get(), std::chrono::steady_clock::now() + std::chrono::seconds(5));
    execution::Send(peer.Get(), {{"tools", json::array()}});
  });
  execution::Client remote(
      execution::TargetFromJson({{"client", client}, {"runtime", runtime}}));
  std::string error;
  CHECK(remote.Open(error));
  gateway.join();
  CHECK(JsonValue(request, "sandbox", "") == "readonly");
}

}  // namespace uagent
