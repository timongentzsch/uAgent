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
#include "include/core/limits.h"
#include "include/core/output_buffer.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"
#include "include/media/attachments.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {

// A row for work that ended outside a tool call, in the same shape as a
// call's. The caller adds what links it to the work.
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

void Agent::RecordReceipt(const Headline& headline, json parts) {
  json node = Receipt(headline, "ok", {}, "", "text", std::move(parts));
  node["turn_root"] = turn_root_;
  node = conversation_.RecordEntry(std::move(node));
  Emit(Event{EventId::kMessageChanged, {{"node", EntryNode(node)}}});
}

// A memory saved or lost outside a turn, as the row a memory tool call draws.
void Agent::RecordMemory(const MemoryEvent& event) {
  const bool failed = event.action == "failed";
  const Headline headline =
      failed ? Headline{"Saving memory", "Could not save memory", event.key}
      : event.action == "created"
          ? Headline{"Saving memory", "Saved memory", event.key}
          : Headline{"Updating memory", "Updated memory", event.key};
  json parts = json::array();
  if (!failed && !event.key.empty()) {
    parts.push_back(LinkPart("memory", event.key, "Open memory"));
  }
  json node = Receipt(headline, failed ? "failed" : "ok", {}, event.preview,
                      "markdown", std::move(parts));
  node["memory"] = {{"action", event.action},
                    {"key", event.key},
                    {"automatic", event.automatic}};
  node["turn_root"] = turn_root_;
  node = conversation_.RecordEntry(std::move(node));
  Emit(Event{EventId::kMessageChanged, {{"node", EntryNode(node)}}});
  std::string line = "Memory " + event.action;
  if (!event.key.empty()) line += " · " + event.key;
  if (!event.preview.empty()) line += "\n  " + event.preview;
  Emit(NoticeEvent(
      failed ? PresentationStatus::kFailed : PresentationStatus::kNeutral,
      std::move(line), false));
  ++revision_;
}

// A notice and a presentation record each, and one bounded batch message for
// the model.
void Agent::DeliverActivityCompletions(
    const std::vector<ActivityCompletion>& completions) {
  const size_t delivered = completions.size();
  constexpr size_t kAutomaticBatchBytes = KiB(12);
  std::string batch = "[completed activities; bounded]\n";
  size_t reduced = 0;
  bool first = true;
  for (const ActivityCompletion& completion : completions) {
    const bool succeeded = ExitedOk(completion.status);
    PresentationRecord record;
    record.status = succeeded ? PresentationStatus::kSucceeded
                              : PresentationStatus::kFailed;
    record.title = "Activity";
    std::string label = completion.display_label.empty()
                            ? FirstLine(completion.command)
                            : completion.display_label;
    if (!label.empty()) {
      record.title += " · " + Utf8Trunc(label, kStatusLabelBytes);
    }
    Event display{
        EventId::kActivityCompleted,
        {{"id", completion.activity_id},
         {"kind", ActivityKindName(completion.kind)},
         {"command", Utf8Trunc(completion.command, kActivityCommandBytes)},
         {"status",
          WIFEXITED(completion.status) ? WEXITSTATUS(completion.status) : -1},
         {"output_chars", completion.output.size()}}};
    // Its call's row takes the result; a receipt and a notice say it
    // otherwise.
    if (!FinishCommandRow(EndedOf(completion))) {
      ResultFacts facts;
      facts.exit = ExitFacts(completion.status);
      json node = Receipt(
          {"Running", succeeded ? "Finished" : "Failed",
           Utf8Trunc(label.empty() ? record.title : label, kStatusLabelBytes)},
          succeeded ? "ok" : "failed", facts, completion.output, "tail",
          json::array(
              {LinkPart("activity", completion.activity_id, "Open activity")}));
      node["activity_id"] = completion.activity_id;
      // The full command travels with the record (bounded): the popup and
      // history need it once the supervisor no longer retains the activity.
      node["command"] = Utf8Trunc(completion.command, kActivityCommandBytes);
      node["turn_root"] = turn_root_;
      node = conversation_.RecordEntry(std::move(node));
      Emit(Event{EventId::kMessageChanged, {{"node", EntryNode(node)}}});
      record.title += succeeded ? " completed" : " failed";
      display.presentation = std::move(record);
    }
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

bool Agent::FinishCommandRow(const EndedCommand& ended) {
  std::string id;
  json row;
  for (const auto& [key, facts] : conversation_.DisplayFacts().items()) {
    if (JsonValue(facts, "activity_id", int64_t{0}) == ended.activity_id &&
        JsonValue(facts, "state", "") == "running") {
      id = key;
      row = facts;
      break;
    }
  }
  if (id.empty()) return false;
  ResultFacts facts;
  if (ended.stopped) {
    facts.outcome = "stopped";
  } else {
    facts.exit = ExitFacts(ended.status);
  }
  const char* state = ended.stopped            ? "cancelled"
                      : ExitedOk(ended.status) ? "ok"
                                               : "failed";
  // What it printed before and after it left; a long one keeps its end,
  // where a command says how it went.
  std::string output = JsonValue(row, "output", "") + ended.output;
  if (output.size() > kPreviewChars) {
    output =
        output.substr(Utf8BoundaryAfter(output, output.size() - kPreviewChars));
  }
  const json update = {{"state", state},
                       {"result", ResultJson(facts, state, ended.duration_ms)},
                       {"body_kind", "tail"},
                       {"output", output},
                       {"duration_ms", ended.duration_ms}};
  row.update(update);
  conversation_.RecordDisplay(id, update);
  Emit(Event{EventId::kMessageChanged,
             {{"node", CallNode(row, JsonValue(row, "work", ""))}}});
  return true;
}

bool Agent::DrainActivities(bool start_turn) {
  bool changed = false;
  std::vector<ActivityCompletion> completions =
      ActivityTakeCompletedDetails(processes_);
  if (RemoteWorkspace()) {
    const json result = WorkspaceRequest({{"op", "completions"}});
    if (const json* rows = JsonArray(result, "completions")) {
      for (const json& c : *rows) {
        ActivityCompletion completion;
        completion.activity_id = JsonValue(c, "activity_id", int64_t{0});
        completion.status = JsonValue(c, "status", 0);
        completion.command = JsonValue(c, "command", "");
        completion.output = JsonValue(c, "output", "");
        completion.hint = JsonValue(c, "hint", "");
        completion.display_label = JsonValue(c, "display_label", "");
        completion.started_ms = JsonValue(c, "started_ms", int64_t{0});
        completions.push_back(std::move(completion));
      }
    }
    if (const json* rows = JsonArray(result, "stopped")) {
      for (const json& e : *rows) processes_.Stopped(EndedFromJson(e));
    }
  }
  const bool delivered = !completions.empty();
  if (delivered) {
    DeliverActivityCompletions(completions);
    changed = true;
  }
  if (delivered && start_turn) {
    Arrive(HarnessNote("activity finished", "Its result is above."),
           {{"kind", "activity"}});
  }
  for (const EndedCommand& ended : processes_.TakeStopped()) {
    changed |= FinishCommandRow(ended);
  }
  if (DrainAttachments()) changed = true;
  changed |= TakeSideReplies();
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
  // The kind stays kAttachment: the request pipeline keys its encoded-parts
  // guard and its turn-boundary strip on it. Only the attribution differs,
  // carried as a display fact.
  if (!sourced.empty() && PushAttachments(sourced)) {
    json call_ids = json::array();
    json files = json::array();
    for (const Attachment& attachment : sourced) {
      call_ids.push_back(attachment.source_call_id);
      // The tool's row shows the file, so a client needs its own copy.
      json kept =
          !attachment.asset_id.empty() ? AttachmentDisplayJson(attachment)
          : keep_tool_file_ ? keep_tool_file_(attachment.path, attachment.name)
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
