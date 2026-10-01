// Copyright 2026 Timon Gentzsch

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
#include "include/media/attachments.h"
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
  // Delegated children are durable conversations even though ordinary
  // one-shot `-p` calls remain ephemeral.
  persist_ = !session_file_.empty();
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
      auto next = SteeringState().TakeNextAutoStart();
      if (!next || AbortRequested()) return;
      RunTurns(next->text);
      SaveSession();
    }
  };
  answer_mail();
  PublishChannelState();
  // Background work is observational and never starts a model turn. Keep the
  // process alive long enough to publish completion and drain retained state.
  while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
    uint64_t generation = runtime_.processes.Generation();
    if (!agent_.DrainBackground()) {
      runtime_.processes.WaitForChange(generation);
    }
    agent_.AccountSideUsage();
    answer_mail();
    SaveSession();
  }
  agent_.AccountSideUsage();
  context_.output.Restore();

  std::string ledger = EnvStr("UAGENT_INTERNAL_USAGE_FILE");
  if (!ledger.empty()) {
    std::string error;
    json entry = {
        {"route", agent_.ActiveRoute()},
        {"routes", agent_.RouteUsageJson()},
        {"usage", UsageJson(agent_.SessionUsage())},
        {"statistics", agent_.Statistics()},
        {"parent_turn", EnvLong("UAGENT_INTERNAL_PARENT_TURN", int64_t{0})}};
    if (!AppendPrivateLine(ledger, JsonDump(entry), error)) {
      fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
    }
  }
  std::string answer = agent_.LastText();
  if (!agent_.LastError().empty()) {
    return FinishHeadless("", agent_.LastError(), 1);
  }
  if (answer.empty()) return FinishHeadless("", "agent produced no answer", 1);
  return FinishHeadless(std::move(answer), "", 0);
}

}  // namespace uagent
