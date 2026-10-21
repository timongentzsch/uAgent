// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#include "include/cli/cli.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/core/usage.h"
#include "include/ui/ask_picker.h"
#include "include/ui/display.h"
#include "include/ui/editor.h"
#include "include/ui/presentation.h"
#include "src/app/session_terminal_internal.h"

namespace uagent::session {
namespace {
// The pinned row from the worker's state frame: the working row while a turn
// runs, the session row otherwise -- the same renderers presentation_test pins.
std::string StatusRow(const json& state, bool interrupting,
                      std::string_view verbosity) {
  const int64_t used = JsonValue(state, "context_tokens", int64_t{0});
  const int64_t window = JsonValue(state, "context_window", int64_t{0});
  const std::string route = JsonValue(state, "route", "");
  size_t background = 0, foreground = 0, subagents = 0;
  std::string subagent;
  if (const json* rows = JsonArray(state, "activities")) {
    for (const json& row : *rows) {
      if (JsonValue(row, "status", "") != "running") continue;
      if (JsonValue(row, "kind", "") == "agent") {
        ++subagents;
        std::string progress = JsonValue(row, "progress", "");
        if (!progress.empty()) {
          subagent = JsonValue(row, "id", "") + ": " + progress;
        }
      } else if (JsonValue(row, "detached", false)) {
        ++background;
      } else {
        ++foreground;
      }
    }
  }
  if (JsonValue(state, "turn_active", false)) {
    // The worker's clock, so a terminal attached mid-turn counts from the
    // turn's start and not from its own.
    const int64_t now = NowMillis();
    return ActivityBar(
        {.elapsed = std::chrono::milliseconds(std::max(
             int64_t{0}, now - JsonValue(state, "turn_started_ms", now))),
         .context_used = used,
         .context_window = window,
         .model = route,
         .background = background,
         .foreground = foreground,
         .subagents = subagents,
         .interrupting = interrupting,
         .subagent = std::move(subagent)});
  }
  const json permissions = JsonValue(state, "permissions", json::object());
  return StatusBar(UsageFromJson(JsonValue(state, "usage", json::object())),
                   {.activity = JsonValue(state, "activity", "Connecting"),
                    .context_used = used,
                    .context_window = window,
                    .model = route,
                    .approval = JsonValue(permissions, "effective", "ask"),
                    .verbosity = std::string(verbosity),
                    .background = background + subagents});
}

// What a pending approval or question says above its prompt.
std::string DecisionDescription(const json& pending) {
  std::string description;
  if (const json* approval = JsonObject(pending, "approval")) {
    std::string risks;
    for (const json& risk : JsonValue(*approval, "risks", json::array())) {
      risks += (risks.empty() ? "Risk: " : " · ") +
               JsonValue(risk, "label", std::string());
    }
    if (!risks.empty()) description = AsciiGlyphs(risks) + "\n";
    if (const std::string review = JsonValue(*approval, "review", "");
        !review.empty()) {
      description += review + "\n";
    }
    description += JsonValue(*approval, "mandatory_reason", "") + "\n" +
                   JsonValue(*approval, "preview", "") + "\n";
  }
  if (const json* questions = JsonArray(pending, "questions")) {
    for (const json& question : *questions) {
      description += JsonValue(question, "question", "") + "\n";
      size_t number = 0;
      for (const json& option : question["options"]) {
        description += "  " + std::to_string(++number) + ". " +
                       JsonValue(option, "label", "") + "\n";
      }
    }
    description +=
        "Answer with option numbers or your own words; separate "
        "questions with ;\n";
  }
  return description;
}
}  // namespace

bool Terminal::PresentDecision(const json& pending,
                               const std::string& decision) {
  if (!decision.empty() && JsonValue(pending, "kind", "") == "ask" && raw_) {
    const json answered =
        PickAskAnswers(JsonValue(pending, "questions", json::array()), region_,
                       wake_.read.Get(), [this, decision] {
                         std::lock_guard lock(mutex_);
                         return JsonValue(pending_, "id", "") == decision;
                       });
    // Answered elsewhere meanwhile: nothing left to send.
    bool live = false;
    {
      std::lock_guard lock(mutex_);
      live = JsonValue(pending_, "id", "") == decision;
    }
    if (live) {
      if (answered.is_null()) {
        Reply(decision, "", {{"cancelled", true}});
      } else {
        Reply(decision, answered["text"],
              {{"attachments", answered["attachments"]}});
      }
    }
    return true;
  }
  if (!decision.empty() && JsonValue(pending, "kind", "") == "editor") {
    std::string text = JsonValue(pending, "initial", "");
    bool edited =
        raw_ ? composer_.EditTextExternally(text)
             : EditExternalText(text, STDIN_FILENO, kSystemPromptBytes);
    Reply(decision, text, {{"cancelled", !edited}});
    return true;
  }
  if (!decision.empty()) {
    const std::string description = DecisionDescription(pending);
    if (raw_) {
      region_.Commit(ColorizeDiffLines(TerminalSafe(description)));
    } else {
      fputs(TerminalSafe(description).c_str(), stdout);
    }
  }
  const std::string prompt = TerminalSafe(
      DecisionPrompt(JsonValue(pending, "prompt", ""),
                     JsonValue(pending, "options", json::array())));
  if (raw_) {
    if (!decision.empty()) {
      draft_ = composer_.Buffer();
      region_.Commit(prompt);
      composer_.Mount(InputPrompt(), JsonValue(pending, "initial", ""), false);
    } else {
      composer_.Mount(InputPrompt(), draft_);
    }
  } else if (!decision.empty()) {
    const char* label = !g_plain ? ""
                        : JsonValue(pending, "kind", "") == "ask"
                            ? "question: "
                            : "approval needed: ";
    printf("%s%s\n", label, prompt.c_str());
  }
  return false;
}

void Terminal::Absorb() {
  auto update = output_.Read();
  if (update.changed) {
    region_.Commit(std::move(update.committed));
    tail_ = std::move(update.tail);
    region_.SetTail(tail_);
  }
}

void Terminal::Paint() {
  Absorb();
  json state;
  {
    std::lock_guard lock(mutex_);
    state = state_;
  }
  if (!JsonValue(state, "turn_active", false)) interrupting_ = false;
  region_.SetStatus(
      StatusBarLine(StatusRow(state, interrupting_, Detail().level)));
  const RawComposer::Layout draft = composer_.View();
  region_.SetComposer(draft.rows, draft.caret_row, draft.caret_col);
  region_.Flush();
}
}  // namespace uagent::session
