// Copyright 2026 Timon Gentzsch

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/agent/process.h"
#include "include/app/bootstrap.h"
#include "include/app/commands.h"
#include "include/app/runtime.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/mcp/rpc.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/tools/memory.h"
#include "include/tools/subagent.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/sessions.h"
#include "src/app/application_internal.h"

namespace uagent {
int Application::FinishInteractive(int status) {
  SaveSession();
  Teardown(exit_reason_.c_str());
  TerminalRestore();
  return status;
}

int Application::RunChannel() {
  if (!ResumeAtStartup()) return FinishInteractive(2);
  persist_ = true;
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
    return ActivityControl(runtime_.processes, request, &runtime_.collaborator);
  });
  PublishChannelState();
  while (std::optional<ApplicationInput> input = channel_->NextInput()) {
    agent_.DrainBackground();
    bool quit = false;
    request_id_ = input->request_id;
    if (input->title) {
      agent_.Rename(std::move(*input->title));
    } else if (!input->control.is_null()) {
      AppSession session = Session();
      json result = SessionControl(session, input->control);
      SaveSession(true);
      channel_->CompleteControl(input->request_id, result);
    } else if (!input->wake) {
      handoff_budget_ = std::move(input->budget);
      for (auto& attachment : input->attachments) {
        attachments_.push_back(std::move(attachment));
      }
      quit = ProcessInput(std::move(input->text));
    }
    SaveSession(input->title.has_value());
    PublishChannelState();
    if (quit) break;
  }
  runtime_.processes.SetNotifyFd(-1);
  channel_->SetActivityControl({});
  return FinishInteractive(0);
}

json Application::BuildChannelState() const {
  json state = InterfaceState();
  state["view"] = agent_.DisplaySnapshot();
  state["usage"] = UsageJson(agent_.SessionUsage());
  state["route_usage"] = agent_.RouteUsageJson();
  state["system_prompt"] = agent_.LastSentPrompt();
  state["statistics"] = agent_.Statistics();
  state["http"] = agent_.HttpExchanges();
  state["permissions"] = PermissionControl(context_, json::object());
  state["efforts"] = json::array({"default"});
  for (const char* effort : kReasoningEfforts) {
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
  state["activities"] = runtime_.processes.ActivityViews();
  state["collaborators"] =
      CollaboratorSummaries(runtime_.processes, &runtime_.collaborator);
  state["error"] = input_error_.empty() ? agent_.LastError() : input_error_;
  state["title"] = Utf8Prefix(agent_.FirstUserText(), 256);
  state["stop"] = agent_.LastStop();
  return state;
}

void Application::PublishChannelState(bool checkpoint) {
  json state = BuildChannelState();
  if (channel_) channel_->PublishState(state, checkpoint);
}

}  // namespace uagent
