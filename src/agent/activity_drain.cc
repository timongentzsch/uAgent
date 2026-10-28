// Copyright 2026 Timon Gentzsch
// What finished in the background reaches the conversation: memory
// extractions, activity completions and pending attachments.

#include <sys/wait.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/agent/activity_supervisor.h"
#include "include/agent/agent.h"
#include "include/agent/conversation.h"
#include "include/agent/memory_store.h"
#include "include/agent/node.h"
#include "include/agent/protocol.h"
#include "include/core/debug.h"
#include "include/core/defaults.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/output_buffer.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {

// A row for work that ended outside a tool call, in the same shape as a
// call's: its headline, how it ended and its output. The caller adds what
// links it to the work.
json Receipt(const Headline& headline, std::string_view state,
             const ResultFacts& facts, std::string text,
             std::string_view body_kind, json parts) {
  json view = {{"verb", {headline.present, headline.past}}};
  if (!headline.target.empty()) view["target"] = headline.target;
  return {{"text", std::move(text)},
          {"view", std::move(view)},
          {"state", state},
          {"result", ResultJson(facts, state, -1)},
          {"body_kind", body_kind},
          {"parts", std::move(parts)},
          {"activity",
           {{"label", headline.target.empty()
                          ? headline.past
                          : headline.past + " " + headline.target}}}};
}

}  // namespace

// One finished extraction: its receipt decides what the memory event records,
// and only a change or a failure is worth a line on screen.
void Agent::ReportMemoryCompletion(ActivityCompletion& completion) {
  bool success = ExitedOk(completion.status);
  MemoryEvent event;
  std::string receipt_error;
  bool receipt_exists =
      !completion.receipt_path.empty() && PathExists(completion.receipt_path);
  bool has_receipt =
      receipt_exists &&
      ReadMemoryReceipt(completion.receipt_path, event, receipt_error);
  if (!success || !has_receipt) {
    event = {};
    if (!success) {
      event.action = "failed";
    } else if (receipt_exists) {
      event.action = "receipt_unavailable";
    } else {
      event.action = "no_change";
    }
    event.source_session = completion.source_id;
    event.timestamp = UtcStamp();
    event.automatic = true;
    if (!success) {
      // The last line, not the first. A child that failed has usually printed
      // a startup warning before it got anywhere -- an over-full memory slice
      // prints one every time -- and taking the head recorded that warning as
      // the cause, which is how eight failures came to be labelled with a
      // condition that did not fail anything. `HeadTail` keeps the
      // same convention: errors live at the end.
      std::string output = RedactMemorySecrets(completion.output);
      std::string_view tail(output);
      while (!tail.empty()) {
        size_t line = tail.find_last_not_of("\r\n");
        if (line == std::string_view::npos) break;
        tail = tail.substr(0, line + 1);
        size_t start = tail.find_last_of('\n');
        std::string_view candidate =
            start == std::string_view::npos ? tail : tail.substr(start + 1);
        if (!Trim(std::string(candidate)).empty()) {
          event.preview = Utf8Trunc(std::string(candidate), kStatusLabelBytes);
          break;
        }
        tail = start == std::string_view::npos ? std::string_view()
                                               : tail.substr(0, start);
      }
    }
    std::string event_error;
    if (!WriteMemoryEvent(event, {}, event_error)) {
      DebugLog("memory_event_write_error", {{"error", event_error}});
    }
  }
  if (!completion.receipt_path.empty()) {
    std::error_code ignored;
    std::filesystem::remove(completion.receipt_path, ignored);
  }

  // Routine outcomes are recorded too, marked minor: a client showing the
  // full picture lists them, the default view does not.
  const bool minor = event.action != "created" && event.action != "updated" &&
                     event.action != "failed" &&
                     event.action != "receipt_unavailable";
  {
    bool warning =
        event.action == "failed" || event.action == "receipt_unavailable";
    std::string label = event.action;
    if (event.action == "no_change") {
      label = "extraction complete · nothing saved";
    } else if (event.action == "receipt_unavailable") {
      label = "extraction complete · receipt unavailable";
    }
    std::string line = "Memory " + label;
    if (!event.key.empty()) line += " · " + event.key;
    // The same row shape as a memory tool call: verb, key, link.
    const Headline headline =
        event.action == "created"
            ? Headline{"Saving memory", "Saved memory", event.key}
        : event.action == "updated"
            ? Headline{"Updating memory", "Updated memory", event.key}
        : event.action == "deleted"
            ? Headline{"Forgetting memory", "Forgot memory", event.key}
        : warning
            ? Headline{"Saving memory", "Could not save memory", event.key}
            : Headline{"Checking memory", "Checked memory", event.key};
    json parts = json::array();
    if (!event.key.empty() && event.action != "deleted") {
      parts.push_back(LinkPart("memory", event.key, "Open memory"));
    }
    ResultFacts facts;
    facts.no_change = event.action == "no_change";
    if (event.action == "receipt_unavailable") facts.outcome = event.action;
    json block = Receipt(headline, warning ? "failed" : "ok", facts,
                         event.preview, "markdown", std::move(parts));
    block["activity"]["category"] = "memory";
    block["memory"] = {{"action", event.action},
                       {"key", event.key},
                       {"automatic", event.automatic}};
    if (minor) block["routine"] = true;
    block["turn_root"] = turn_root_;
    block = conversation_.RecordEntry(std::move(block));
    Emit(Event{EventId::kMessageChanged, {{"block", EntryNode(block)}}});
    // The preview continues the notice, indented under its dot.
    if (!event.preview.empty()) line += "\n  " + event.preview;
    Emit(NoticeEvent(
        warning ? PresentationStatus::kFailed : PresentationStatus::kNeutral,
        std::move(line), minor));
  }
  DebugLog("memory_extract_finished", {{"activity_id", completion.activity_id},
                                       {"action", event.action},
                                       {"key", event.key},
                                       {"source_session", event.source_session},
                                       {"receipt_error", receipt_error}});
}

// Everything that is not a memory extraction: a notice and a presentation
// record each, and one bounded batch message for the model.
void Agent::DeliverActivityCompletions(
    const std::vector<ActivityCompletion>& completions) {
  const size_t delivered = static_cast<size_t>(std::count_if(
      completions.begin(), completions.end(), [](const auto& completion) {
        return completion.kind != ActivityKind::kMemory;
      }));
  constexpr size_t kAutomaticBatchBytes = KiB(12);
  std::string batch = "[completed activities; bounded]\n";
  size_t reduced = 0;
  bool first = true;
  for (const ActivityCompletion& completion : completions) {
    if (completion.kind == ActivityKind::kMemory) continue;
    const bool succeeded = ExitedOk(completion.status);
    PresentationRecord record;
    record.status = succeeded ? PresentationStatus::kSucceeded
                              : PresentationStatus::kFailed;
    record.title =
        completion.kind == ActivityKind::kSubagent ? "Subagent" : "Activity";
    std::string label = completion.display_label.empty()
                            ? FirstLine(completion.command)
                            : completion.display_label;
    if (!label.empty()) {
      record.title += " · " + Utf8Trunc(label, kStatusLabelBytes);
    }
    const bool agent = completion.kind == ActivityKind::kSubagent &&
                       !completion.source_id.empty();
    ResultFacts facts;
    facts.exit = ExitFacts(completion.status);
    json block = Receipt(
        {agent ? "Delegating" : "Running",
         succeeded ? (agent ? "Delegated" : "Finished") : "Failed",
         Utf8Trunc(label.empty() ? record.title : label, kStatusLabelBytes)},
        succeeded ? "ok" : "failed", facts, completion.output, "tail",
        json::array({agent
                         ? LinkPart("agent", completion.source_id, "Open agent")
                         : LinkPart("activity", completion.activity_id,
                                    "Open activity")}));
    block["activity"]["category"] =
        completion.kind == ActivityKind::kSubagent ? "delegate" : "run";
    block["activity_id"] = completion.activity_id;
    // The full command travels with the record (bounded): rows and titles
    // abbreviate, but the popup and history must not lose it when the
    // supervisor no longer retains the activity.
    block["command"] = Utf8Trunc(completion.command, kActivityCommandBytes);
    block["agent_id"] = completion.source_id;
    block["turn_root"] = turn_root_;
    block = conversation_.RecordEntry(std::move(block));
    Emit(Event{EventId::kMessageChanged, {{"block", EntryNode(block)}}});
    Event display{
        EventId::kActivityCompleted,
        {{"id", completion.activity_id},
         {"kind", ActivityKindName(completion.kind)},
         {"command", Utf8Trunc(completion.command, kActivityCommandBytes)},
         {"status",
          WIFEXITED(completion.status) ? WEXITSTATUS(completion.status) : -1},
         {"output_chars", completion.output.size()}}};
    record.title += succeeded ? " completed" : " failed";
    display.presentation = std::move(record);
    Emit(std::move(display));

    std::string note = ActivityCompletionText(completion, completion.output);
    size_t separator = first ? 0 : 2;
    if (batch.size() + separator + note.size() > kAutomaticBatchBytes) {
      note = ActivityCompletionText(
          completion, HeadTail(completion.output, 512) +
                          "\n[completion reduced; use activity for the "
                          "retained transcript]");
      ++reduced;
    }
    if (!first) batch += "\n\n";
    first = false;
    batch += note;
  }
  if (delivered > 0) {
    conversation_.Push(HarnessMessage(std::move(batch)),
                       MessageKind::kInternal);
  }
  DebugLog("background_results_delivered",
           {{"count", delivered}, {"reduced", reduced}});
}

bool Agent::DrainActivities(bool start_turn) {
  bool changed = false;
  // Take one snapshot. A memory child can become drainable at any instant; two
  // separate takes let the generic pass steal a child that completed just
  // after the memory-only pass, bypassing its receipt and audit handling.
  std::vector<ActivityCompletion> completions =
      ActivityTakeCompletedDetails(processes_);
  for (ActivityCompletion& completion : completions) {
    if (completion.kind == ActivityKind::kMemory) {
      ReportMemoryCompletion(completion);
    }
  }
  const bool delivered = std::any_of(
      completions.begin(), completions.end(), [](const auto& completion) {
        return completion.kind != ActivityKind::kMemory;
      });
  if (delivered) {
    DeliverActivityCompletions(completions);
    changed = true;
  }
  if (delivered && start_turn) {
    Arrive(HarnessNote("activity finished", "Its result is above."));
  }
  if (DrainAttachments()) changed = true;
  {
    std::lock_guard lock(title_mutex_);
    if (!generated_title_.empty() && !custom_title_) {
      session_title_ = std::move(generated_title_);
      changed = true;
    }
    generated_title_.clear();
  }
  if (changed) ++revision_;
  return changed;
}

bool Agent::DrainAttachments() {
  std::vector<Attachment> pending = Attachments().Take();
  if (pending.empty()) return false;
  // Origin decides attribution. Real user uploads render as the user's own
  // turn; files a tool read ride the model context as harness-owned context
  // and display on the tool's own row, never as a fake user message.
  std::vector<Attachment> user, sourced;
  for (Attachment& attachment : pending) {
    (attachment.source_call_id.empty() ? user : sourced)
        .push_back(std::move(attachment));
  }
  if (!user.empty()) PushAttachments(user);
  // The message kind stays kAttachment on purpose: the request pipeline keys
  // its encoded-parts guard and its turn-boundary strip on it, so re-kinding
  // would feed base64 to the summarizer. Only the attribution differs, carried
  // as a display fact the view projects as agent-side.
  if (!sourced.empty() && PushAttachments(sourced)) {
    json call_ids = json::array();
    json files = json::array();
    for (const Attachment& attachment : sourced) {
      call_ids.push_back(attachment.source_call_id);
      // The tool's row shows the file, so a client needs its own copy.
      json kept = keep_tool_file_
                      ? keep_tool_file_(attachment.path, attachment.name)
                      : json(nullptr);
      if (kept.is_object()) {
        kept["image"] = attachment.image;
        files.push_back(std::move(kept));
      }
    }
    json facts = {{"origin", "tool"}, {"source_call_ids", std::move(call_ids)}};
    if (!files.empty()) facts["files"] = std::move(files);
    conversation_.RecordDisplay(conversation_.LastDisplayId(),
                                std::move(facts));
  }
  DebugLog(
      "attachments_added",
      {{"turn", turn_id_}, {"user", user.size()}, {"sourced", sourced.size()}});
  return true;
}

bool Agent::PushAttachments(const std::vector<Attachment>& attachments) {
  std::string error;
  json content = AttachmentContent(kAttachedOnRequest, attachments, error);
  if (!error.empty()) {
    conversation_.Push(HarnessMessage("[attachment failed] " + error),
                       MessageKind::kInternal);
    return false;
  }
  conversation_.Push({{"role", "user"}, {"content", std::move(content)}},
                     MessageKind::kAttachment);
  return true;
}

}  // namespace uagent
