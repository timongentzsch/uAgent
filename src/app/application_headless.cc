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
#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/tools/memory.h"
#include "include/agent/process.h"
#include "include/tools/subagent.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/sessions.h"
#include "src/app/application_internal.h"

namespace uagent {
int Application::FinishHeadless(std::string answer, std::string error,
                                int exit_code) {
  Teardown(exit_code == 0 ? "headless_complete" : "headless_error");
  if (context_.options.json_stream || context_.options.json) {
    json envelope =
        HeadlessResult(std::move(answer), std::move(error),
                       agent_.LatestToolTrace(), agent_.SessionUsage(),
                       agent_.RouteUsageJson(), exit_code, agent_.LastStop());
    if (context_.options.json_stream) {
      Emit(Event{exit_code == 0 ? EventId::kAnswer : EventId::kError,
                 std::move(envelope)});
    } else {
      printf("%s\n", JsonDump(envelope).c_str());
    }
  } else if (exit_code == 0) {
    printf("%s\n", TerminalSafe(answer).c_str());
  } else {
    fprintf(stderr, "%s\n", TerminalSafe(error).c_str());
  }
  return exit_code;
}

int Application::RunHeadless() {
  // Internal collaborator sessions are explicit durable conversations even
  // though ordinary one-shot `-p` calls remain ephemeral.
  persist_ = !session_file_.empty();
  json content;
  if (!attachments_.empty()) {
    std::string error;
    content = AttachmentContent(context_.options.prompt, attachments_, error);
    if (!error.empty()) {
      context_.output.Restore();
      return FinishHeadless("", std::move(error), 2);
    }
  }
  RunTurns(context_.options.prompt, std::move(content));
  SaveSession();
  PublishChannelState();
  // Background work is observational and never starts a model turn. Keep the
  // process alive long enough to publish completion and drain retained state.
  while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
    uint64_t generation = runtime_.processes.Generation();
    if (!agent_.DrainBackground()) {
      runtime_.processes.WaitForChange(generation);
    }
    SaveSession();
  }
  context_.output.Restore();

  std::string ledger = EnvStr("UAGENT_USAGE_FILE");
  if (!ledger.empty()) {
    std::string error;
    json entry = {{"route", agent_.ActiveRoute()},
                  {"routes", agent_.RouteUsageJson()},
                  {"usage", UsageJson(agent_.SessionUsage())}};
    if (!AppendPrivateLine(ledger, JsonDump(entry), error)) {
      fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
    }
  }
  std::string answer = agent_.LastText();
  if (!agent_.LastError().empty()) {
    return FinishHeadless("", agent_.LastError(), 1);
  }
  if (answer.empty()) {
    std::string error = agent_.LastError().empty() ? "agent produced no answer"
                                                   : agent_.LastError();
    return FinishHeadless("", std::move(error), 1);
  }
  return FinishHeadless(std::move(answer), "", 0);
}

}  // namespace uagent
