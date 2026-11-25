// Copyright 2026 Timon Gentzsch

#include <string>
#include <string_view>
#include <utility>

#include "include/agent/session_store.h"
#include "include/cli/cli.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/workspace.h"
#include "include/mcp/register.h"
#include "include/providers/providers.h"
#include "include/tools/memory.h"
#include "src/app/application_internal.h"

namespace uagent {
int Application::RunChannel() {
  if (std::string error = ResumeAtStartup(); !error.empty()) {
    // No teardown: its save would write this empty conversation over the
    // file once the runtime holding it lets go.
    Emit(Event{EventId::kError, {{"error", "cannot resume: " + error}}});
    return 2;
  }
  persist_ = true;
  agent_.GenerateTitles(true);
  EnsureSessionPath();
  if (!PathExists(session_file_) && !channel_->InitialTitle().empty()) {
    agent_.Rename(channel_->InitialTitle());
  }
  SaveSession(true);
  used_before_ = UsageTotals();
  if (!RemoteWorkspace() && AgentDepth() == 0 && api_.config.memory_enabled &&
      api_.config.memory_generate) {
    std::string error = StartMemoryExtractor(
        agent_, api_, CanonicalAccessPath(CanonicalCwd()), session_file_);
    if (!error.empty()) {
      DebugLog("memory_extract_start_error", {{"error", error}});
    }
  }
  runtime_.processes.SetNotifyFd(channel_->WakeFd());
  channel_->SetActivityControl([this](const json& request) {
    if (JsonValue(request, "kind", "") == "permissions") {
      return PermissionControl(context_, request);
    }
    if (JsonValue(request, "kind", "") == "tools") {
      return agent_.PublishedCatalogue();
    }
    if (JsonValue(request, "kind", "") == "side") {
      if (JsonValue(request, "operation", "") != "create") {
        return json{{"error",
                     "Update or reload your client to start a "
                     "tool-capable side conversation."}};
      }
      json side =
          agent_.SideConversation(session_file_, JsonValue(request, "text", ""),
                                  JsonValue(request, "request", ""));
      side["side_question"] = JsonValue(request, "text", "");
      return side;
    }
    return ActivityControl(runtime_.processes, request);
  });
  // Mail a previous runtime took but never saved is delivered again, and
  // mail that arrived while none ran starts a turn now.
  RecoverMail(SessionIdFor(session_file_));
  if (agent_.DeliverMail()) SaveSession(false);
  PublishChannelState();
  while (std::optional<ApplicationInput> input = channel_->NextInput()) {
    // An idle session takes background work's result up at once, as a turn.
    runtime_.processes.Tick(true);
    agent_.DrainActivities(input->wake);
    agent_.DeliverMail();
    agent_.AccountSideUsage();
    request_id_ = input->request_id;
    origin_ = input->origin;
    report_ = input->report;
    json result;
    if (input->title) {
      agent_.Rename(std::move(*input->title));
    } else if (input->keep) {
      agent_.KeepHere();
    } else if (!input->control.is_null()) {
      AppSession session = Session();
      result = SessionControl(session, input->control);
    } else if (!input->wake) {
      for (auto& attachment : input->attachments) {
        attachments_.push_back(std::move(attachment));
      }
      ProcessInput(std::move(input->text));
      // A merge's summary written to its end goes to the conversation.
      if (report_ &&
          JsonValue(agent_.LastStop(), "reason", "") == "completed") {
        agent_.MarkMerged();
      }
      // Mail that came as the turn was ending starts the next one.
      agent_.DeliverMail();
      // An agent's spend is its parent's too, reported before the turn's
      // end wakes the parent; so is a side chat's.
      const json agent = context_.options.AgentRole();
      if (const std::string parent = agent.empty()
                                         ? agent_.SideParent()
                                         : JsonValue(agent, "parent", "");
          !parent.empty()) {
        ReportUsage(UsageLedger(parent));
      }
    }
    RunQueuedCommands();
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
    state["http"] = agent_.HttpExchanges();
    // An agent's last answer whole, for the report to its parent, and a
    // merged side chat's: the view holds only a long one's opening.
    if (!context_.options.AgentRole().empty() || report_) {
      constexpr size_t kAnswerBytes = kMailBytes / 4;
      std::string answer = agent_.LastText();
      if (answer.size() > kAnswerBytes) {
        answer = Utf8Prefix(answer, kAnswerBytes) +
                 "\n[cut short here; agents read shows the rest]";
      }
      state["answer"] = std::move(answer);
    }
  }
  state["usage"] = UsageJson(agent_.SessionUsage());
  state["statistics"] = agent_.Statistics();
  state["permissions"] = PermissionControl(context_, json::object());
  state["mcp"] = McpStatus(runtime_.mcp, context_.tools);
  state["efforts"] = json::array({"default"});
  for (std::string_view effort : kReasoningEfforts) {
    if (SupportsReasoningEffort(api_, effort)) {
      state["efforts"].push_back(effort);
    }
  }
  state["turns"] = agent_.UserTurns();
  state["side"] = SideOf(agent_.History().DisplayFacts());
  state["activities"] = runtime_.processes.ActivityViews();
  state["todo"] = todo_ = agent_.Todo();
  state["error"] = input_error_.empty() ? agent_.LastError() : input_error_;
  state["title"] = Utf8Prefix(agent_.FirstUserText(), kUserTitleChars);
  state["suggestion"] = suggestion_ = agent_.Suggestion();
  state["stop"] = agent_.LastStop();
  if (channel_) channel_->PublishState(state, checkpoint);
}

}  // namespace uagent
