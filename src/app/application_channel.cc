// Copyright 2026 Timon Gentzsch

#include <string>
#include <string_view>
#include <utility>

#include "include/agent/child_agent.h"
#include "include/cli.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/register.h"
#include "include/providers.h"
#include "include/tools/memory.h"
#include "include/tools/subagent.h"
#include "src/app/application_internal.h"

namespace uagent {
int Application::RunChannel() {
  if (!ResumeAtStartup()) {
    Teardown("eof");
    return 2;
  }
  persist_ = true;
  agent_.GenerateTitles(true);
  EnsureSessionPath();
  if (!PathExists(session_file_) && !channel_->InitialTitle().empty()) {
    agent_.Rename(channel_->InitialTitle());
  }
  SaveSession(true);
  if (AgentDepth() == 0 && api_.config.memory_enabled &&
      api_.config.memory_generate) {
    std::string error = StartMemoryExtractor(
        runtime_.processes, api_, CanonicalAccessPath(CanonicalCwd()),
        session_file_);
    if (!error.empty()) {
      DebugLog("memory_extract_start_error", {{"error", error}});
    }
  }
  runtime_.processes.SetNotifyFd(channel_->WakeFd());
  channel_->SetActivityControl([this](const json& request) {
    if (JsonValue(request, "kind", "") == "permissions") {
      return PermissionControl(context_, request);
    }
    if (JsonValue(request, "kind", "") == "side") {
      json answer = agent_.SideQuestion(JsonValue(request, "text", ""));
      if (const json* usage = JsonObject(answer, "usage")) {
        runtime_.side_usage.Add(*usage);
      }
      return answer;
    }
    return ActivityControl(runtime_.processes, request);
  });
  // Mail a previous runtime took but never saved is delivered again, and
  // mail that arrived while none ran starts a turn now.
  RecoverMail(MailboxIdFor(session_file_));
  if (agent_.DeliverMail(channel_->HoldMail())) SaveSession(false);
  PublishChannelState();
  while (std::optional<ApplicationInput> input = channel_->NextInput()) {
    // A child's result arrives on its own; an idle session also takes it up
    // at once, as a turn, since it delegated in order to hear back.
    bool children_finished = false;
    agent_.DrainBackground(&children_finished);
    if (children_finished && input->wake) {
      const std::string note =
          "[subagent finished, not a user message] Its result is above.";
      agent_.NotFromUser(note);
      SteeringState().Queue(note, "", true);
    }
    agent_.DeliverMail(channel_->HoldMail());
    agent_.AccountSideUsage();
    request_id_ = input->request_id;
    json result;
    if (input->title) {
      agent_.Rename(std::move(*input->title));
    } else if (!input->control.is_null()) {
      AppSession session = Session();
      result = SessionControl(session, input->control);
    } else if (!input->wake) {
      for (auto& attachment : input->attachments) {
        attachments_.push_back(std::move(attachment));
      }
      if (input->quiet) {
        agent_.Say(input->text, input->request_id);
      } else {
        ProcessInput(std::move(input->text));
      }
      // Mail that came as the turn was ending starts the next one.
      agent_.DeliverMail(channel_->HoldMail());
    }
    SaveSession(input->title.has_value());
    if (!input->title && !input->control.is_null()) {
      channel_->CompleteControl(input->request_id, result);
    }
    PublishChannelState();
  }
  runtime_.processes.SetNotifyFd(-1);
  channel_->SetActivityControl({});
  Teardown("eof");
  return 0;
}

void Application::PublishChannelState(bool checkpoint) {
  json state = InterfaceState();
  // Clients keep these from the last checkpoint (session::kCheckpointFields).
  if (checkpoint) {
    state["view"] = agent_.DisplaySnapshot();
    state["system_prompt"] = agent_.LastSentPrompt();
    state["http"] = agent_.HttpExchanges();
  }
  state["view_epoch"] = agent_.ViewEpoch();
  // The agent's own self-directive, which the Instructions screen can clear.
  if (!runtime_.adaptive_system.instructions.empty()) {
    const AdaptiveSystemState& self = runtime_.adaptive_system;
    state["self_directive"] = {{"mode", self.mode},
                               {"text", self.instructions},
                               {"revision", std::to_string(self.revision)}};
  }
  state["usage"] = UsageJson(agent_.SessionUsage());
  state["route_usage"] = agent_.RouteUsageJson();
  state["statistics"] = agent_.Statistics();
  state["permissions"] = PermissionControl(context_, json::object());
  state["mcp"] = McpStatus(runtime_.mcp, context_.tools);
  state["efforts"] = json::array({"default"});
  for (std::string_view effort : kReasoningEfforts) {
    if (SupportsReasoningEffort(api_, effort)) {
      state["efforts"].push_back(effort);
    }
  }
  state["variants"] = json::array();
  if (api_.capabilities.model_variants) {
    state["variants"].push_back("default");
    for (std::string_view variant : kOpenRouterVariants) {
      state["variants"].push_back(variant);
    }
  }
  state["turns"] = agent_.UserTurns();
  state["turn_active"] = turn_active_;
  state["chat_round"] = agent_.ChatRound();
  state["activities"] = runtime_.processes.ActivityViews();
  state["agents"] = AgentSummaries(runtime_.processes);
  state["error"] = input_error_.empty() ? agent_.LastError() : input_error_;
  state["title"] = Utf8Prefix(agent_.FirstUserText(), 256);
  state["stop"] = agent_.LastStop();
  if (channel_) channel_->PublishState(state, checkpoint);
}

}  // namespace uagent
