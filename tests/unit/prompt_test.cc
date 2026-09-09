// Copyright 2026 Timon Gentzsch
#include "include/agent/prompt.h"

#include <string>
#include <vector>

#include "include/agent.h"
#include "include/app/prompt_control.h"
#include "include/tools/adapt_system.h"
#include "include/tools/files.h"
#include "tests/unit/test_support.h"

namespace uagent {
void TestPromptScopes() {
  TestWorkspace workspace("prompt-scopes");
  AdaptiveSystemState state;
  const json context = {{{"scope", "runtime"}, {"text", "host facts"}}};
  auto control = [&](const json& request) {
    return PromptControl(request, &state, "built-in", context);
  };
  auto change = [&](const std::string& scope, const std::string& mode,
                    const std::string& text) {
    const auto current = control({{"scope", scope}});
    return control({{"action", "set"},
                    {"scope", scope},
                    {"mode", mode},
                    {"text", text},
                    {"revision", current["item"]["revision"]}});
  };
  CHECK(control({})["effective"] == "built-in\n\nhost facts");
  CHECK(change("global", "overlay", "global")["effective"] ==
        "built-in\n\nglobal\n\nhost facts");
  CHECK(change("project", "replace", "project")["effective"] ==
        "project\n\nhost facts");
  CHECK(change("conversation", "overlay", "conversation")["effective"] ==
        "project\n\nconversation\n\nhost facts");
  auto current = control({});
  CHECK(current["sources"][0]["active"] == false);
  CHECK(current["sources"][1]["active"] == false);
  CHECK(control({})["digest"] == current["digest"]);
  auto reset =
      control({{"action", "reset"}, {"revision", current["item"]["revision"]}});
  CHECK(reset["effective"] == "project\n\nhost facts");
  CHECK(control({{"action", "set"},
                 {"mode", "replace"},
                 {"text", "stale"},
                 {"revision", current["item"]["revision"]}})["conflict"] ==
        true);
  auto preview = control({{"action", "preview"},
                          {"mode", "replace"},
                          {"text", "新\n\ntext"},
                          {"revision", reset["item"]["revision"]}});
  CHECK(control({})["effective"] == reset["effective"]);
  CHECK(change("conversation", "replace", "新\n\ntext")["effective"] ==
        preview["effective"]);
  CHECK(change("conversation", "replace", "")["effective"] == "host facts");
  CHECK(change("conversation", "overlay",
               std::string(kAdaptiveSystemBytes + 1, 'x'))
            .contains("error"));
  change("conversation", "replace", "unique value");
  auto revision = control({})["item"]["revision"];
  auto edited = control({{"action", "edit"},
                         {"old", "unique"},
                         {"new", "edited"},
                         {"revision", revision}});
  CHECK(edited["effective"] == "edited value\n\nhost facts");
  CHECK(control({{"action", "edit"},
                 {"old", "missing"},
                 {"new", "x"},
                 {"revision", edited["item"]["revision"]}})
            .contains("error"));
  auto tool = AdaptSystemTool(state, control);
  auto global = control({{"scope", "global"}});
  json args = {{"action", "set"},
               {"scope", "global"},
               {"mode", "replace"},
               {"text", "approved"},
               {"revision", global["item"]["revision"]},
               {"reason", "explicit user request"}};
  CHECK(RequiredApproval(tool, args) == ApprovalClass::kMandatoryHuman);
  CHECK(!tool.run(args, {}).Ok());
  CHECK(tool.approval_preview(args).find("approved") != std::string::npos);
  CHECK(tool.run(args, {}).Ok());
  CHECK(!tool.run(args, {}).Ok());  // single-use approval
  args["revision"] = control({{"scope", "global"}})["item"]["revision"];
  tool.approval_preview(args);
  change("project", "replace", "changed during approval");
  CHECK(!tool.run(args, {}).Ok());
  // A malformed external edit is reported, preserved and explicitly repairable.
  CHECK(ToolWritePrivateFile(PromptDocumentPath("project"), "not json").Ok());
  auto invalid = control({{"scope", "project"}});
  CHECK(invalid.contains("error"));
  CHECK(
      PromptCommand("edit --scope project", control, false).contains("error"));
  CHECK(control({{"scope", "project"},
                 {"action", "reset"},
                 {"revision", invalid["item"]["revision"]}})
            .contains("effective"));
}

void TestPromptRequestParity() {
  TestWorkspace workspace("prompt-request");
  AdaptiveSystemState state;
  Api api(RuntimeConfig{});
  std::vector<Tool> tools;
  ProcessSupervisor processes;
  UsageAccumulator usage;
  Agent agent(
      api, tools, processes, usage,
      [](const Tool&, const json&) { return false; }, {}, {}, {}, &state);
  auto initial = agent.PromptConfiguration({});
  auto proposed = agent.PromptConfiguration(
      {{"action", "set"},
       {"mode", "replace"},
       {"text", "Only this behavior.\nPreserve whitespace."},
       {"revision", initial["item"]["revision"]}});
  CHECK(proposed["effective"].get<std::string>().find("Gather only") ==
        std::string::npos);
  agent.PreviewContext();
  CHECK(agent.ModelRequest()["messages"][0]["content"] ==
        proposed["effective"]);
  const auto stable = agent.ModelRequest();
  agent.PreviewContext();
  CHECK(agent.ModelRequest() == stable);
  const auto path = (workspace.workspace / "session.json").string();
  std::string error;
  CHECK(agent.Save(path, error));
  state.mode = "overlay";
  state.instructions.clear();
  CHECK(agent.Load(path, workspace.workspace.string(), error));
  CHECK(state.mode == "replace");
  CHECK(agent.PromptConfiguration({})["effective"] == proposed["effective"]);
  CHECK(ToolWritePrivateFile(PromptDocumentPath("project"), "invalid").Ok());
  CHECK(agent.PreviewContext().contains("error"));
}
}  // namespace uagent
