// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/media/attachments.h"
#include "src/app/application_internal.h"

namespace uagent {
namespace {

// A session's totals less what they were at `before`, route by route.
json UsedSince(const Agent& agent, const json& before) {
  const auto since = [](const json& now, const json& then) {
    return UsageJson(UsageDifference(UsageFromJson(now), UsageFromJson(then)));
  };
  const json routes_now = agent.RouteUsageJson();
  json routes = json::object();
  for (const auto& [route, used] : routes_now.items()) {
    json spent =
        since(used, JsonValue(before["routes"], route.c_str(), json::object()));
    if (HasUsage(UsageFromJson(spent))) routes[route] = std::move(spent);
  }
  const json& statistics_now = agent.Statistics();
  json statistics = json::object();
  for (const auto& [key, value] : statistics_now.items()) {
    if (!value.is_number()) continue;
    const json& then = before["statistics"];
    if (value.is_number_float()) {
      statistics[key] = std::max(
          0.0, value.get<double>() - JsonValue(then, key.c_str(), 0.0));
    } else {
      statistics[key] =
          std::max(int64_t{0}, value.get<int64_t>() -
                                   JsonValue(then, key.c_str(), int64_t{0}));
    }
  }
  return {{"route", agent.ActiveRoute()},
          {"routes", std::move(routes)},
          {"usage", since(UsageJson(agent.SessionUsage()), before["usage"])},
          {"statistics", std::move(statistics)}};
}

}  // namespace

int Application::FinishHeadless(std::string answer, std::string error,
                                int exit_code) {
  Teardown(exit_code == 0 ? "headless_complete" : "headless_error");
  // What this run spent, after teardown has taken in what its own children
  // spent: the envelope reports it, and a child writes it for its parent on
  // every way out.
  const json used = used_before_.is_object() ? UsedSince(agent_, used_before_)
                                             : json(nullptr);
  if (used.is_object() && !EnvStr("UAGENT_INTERNAL_USAGE_FILE").empty()) {
    std::string failure;
    json entry = used;
    entry["parent_turn"] = EnvLong("UAGENT_INTERNAL_PARENT_TURN", int64_t{0});
    if (!AppendPrivateLine(EnvStr("UAGENT_INTERNAL_USAGE_FILE"),
                           JsonDump(entry), failure)) {
      fprintf(stderr, "cannot write usage ledger: %s\n", failure.c_str());
    }
  }
  if (context_.options.json_stream || context_.options.json) {
    json stop = agent_.LastStop();
    if (out_of_time_ && stop.is_object()) {
      stop["reason"] = "turn_deadline";
      stop["detail"] = "time limit reached with work it started still running";
    }
    json envelope = HeadlessResult(
        std::move(answer), std::move(error), agent_.LatestToolTrace(),
        used.is_object() ? UsageFromJson(used["usage"]) : agent_.SessionUsage(),
        used.is_object() ? used["routes"] : agent_.RouteUsageJson(), exit_code,
        std::move(stop));
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
  // Saved like any conversation, so `-c -p` continues it.
  persist_ = true;
  // Spend is reported from here on: a continued conversation's envelope
  // carries this run, not everything before it.
  used_before_ = {{"usage", UsageJson(agent_.SessionUsage())},
                  {"routes", agent_.RouteUsageJson()},
                  {"statistics", agent_.Statistics()}};
  // A delegated child stops when told to and keeps to its time limit as a
  // whole, not only turn by turn.
  auto deadline = std::chrono::steady_clock::time_point::max();
  if (!EnvStr("UAGENT_INTERNAL_USAGE_FILE").empty()) {
    SetGracefulShutdown(true, /*stop_started=*/true);
    if (api_.config.max_turn_seconds > 0) {
      deadline = DeadlineAfter(api_.config.max_turn_seconds);
    }
  }
  const auto stopping = [&] {
    return AbortRequested() || ShutdownRequested() ||
           std::chrono::steady_clock::now() >= deadline;
  };
  // A followup run first receives what the previous run took but never saved.
  RecoverMail(MailboxIdFor(session_file_));
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
  // Mail that arrived after the last step (a parent's guidance) runs another
  // turn instead of waiting in the mailbox after this process is gone.
  auto answer_mail = [&] {
    for (;;) {
      agent_.DeliverMail();
      if (stopping()) return;
      auto next = SteeringState().TakeNextAutoStart();
      if (!next) return;
      // A later turn gets what is left of the child's time, not a new share.
      if (deadline != std::chrono::steady_clock::time_point::max()) {
        api_.config.max_turn_seconds = std::max<int64_t>(
            1, std::chrono::duration_cast<std::chrono::seconds>(
                   deadline - std::chrono::steady_clock::now())
                   .count());
      }
      origin_ = next->origin;
      RunTurns(next->text);
      SaveSession();
    }
  };
  answer_mail();
  PublishChannelState();
  // Background work is observational and never starts a model turn. Keep the
  // process alive long enough to publish completion and drain retained state.
  while (runtime_.processes.JoinableCount() > 0 && !stopping()) {
    uint64_t generation = runtime_.processes.Generation();
    if (!agent_.DrainActivities()) {
      (void)runtime_.processes.WaitForChange(generation, deadline);
    }
    agent_.AccountSideUsage();
    answer_mail();
    SaveSession();
  }
  out_of_time_ = runtime_.processes.JoinableCount() > 0 &&
                 std::chrono::steady_clock::now() >= deadline;
  agent_.AccountSideUsage();
  context_.output.Restore();

  std::string answer = agent_.LastText();
  if (!agent_.LastError().empty()) {
    return FinishHeadless("", agent_.LastError(), 1);
  }
  if (answer.empty()) return FinishHeadless("", "agent produced no answer", 1);
  return FinishHeadless(std::move(answer), "", 0);
}

}  // namespace uagent
