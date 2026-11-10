// Copyright 2026 Timon Gentzsch
#include <string>

#include "include/app/execution.h"
#include "include/app/launch.h"
#include "include/core/workspace.h"
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
  original.effect =
      FileEffect{"/remote/file", true, std::string("before"), "hash", 0600};
  original.background = 42;
  original.ended.push_back({43, 0, "child done", 123.0, false});
  original.facts.exit = ResultFacts::Exit{7, 0};
  original.parts = json::array({{{"kind", "file"}, {"name", "report"}}});
  const ToolResult copy =
      execution::DecodeResult(execution::EncodeResult(original));
  CHECK(copy.output == original.output);
  CHECK(copy.effect && copy.effect->before == original.effect->before);
  CHECK(copy.effect && copy.effect->before_mode == 0600);
  CHECK(copy.background == 42 && copy.ended.size() == 1);
  CHECK(copy.facts.exit && copy.facts.exit->code == 7);
  CHECK(copy.parts == original.parts);
  CHECK(execution::DecodeResult({{"error", "Disconnected"}}).error ==
        ToolErrorCode::kUnavailable);
}
}  // namespace uagent
