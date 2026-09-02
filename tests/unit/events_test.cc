// Copyright 2026 Timon Gentzsch

#include "include/core/events.h"

#include <sys/stat.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/reference.h"
#include "include/cli.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestObservabilityEvents() {
  json manifest = ReferenceManifestJson();
  json provenance = BuildProvenanceJson();
  CHECK(JsonValue(provenance, "format", 0) == 1);
  CHECK(provenance["binary_version"] == manifest["version"]);
  CHECK(provenance["prompt_digest"] == manifest["prompt_digest"]);
  CHECK(JsonValue(provenance, "build_id", "").size() == 16);

  // The public_type strings are the --json-stream contract that external
  // consumers parse, so they are pinned here rather than left to the table.
  CHECK(std::string(PolicyFor(EventId::kTurnStarted).public_type) ==
        "turn.started");
  CHECK(std::string(PolicyFor(EventId::kResponseStarted).app_type) ==
        "response.started");
  CHECK(std::string(PolicyFor(EventId::kApprovalResolved).app_type) ==
        "approval.resolved");
  CHECK(std::string(PolicyFor(EventId::kToolCall).public_type) == "tool.call");
  CHECK(std::string(PolicyFor(EventId::kSessionResumed).journal_type) ==
        "session.resumed");
  CHECK(std::string(PolicyFor(EventId::kToolResult).public_type) ==
        "tool.result");
  CHECK(std::string(PolicyFor(EventId::kTurnCompleted).public_type) == "usage");
  CHECK(PolicyFor(EventId::kTurnStopped).public_type == nullptr);
  CHECK(PolicyFor(EventId::kReasoningDelta).durability ==
        EventDurability::kTransient);
  CHECK(PolicyFor(EventId::kToolResult).durability ==
        EventDurability::kDurable);
  CHECK(PolicyFor(EventId::kActivityCompleted).durability ==
        EventDurability::kDurable);

  // Notices used to be raw printf, so they reached a terminal and nothing
  // else. They are durable now: journalled and projected to the public JSONL.
  CHECK(std::string(PolicyFor(EventId::kNotice).journal_type) == "notice");
  CHECK(std::string(PolicyFor(EventId::kNotice).public_type) == "notice");
  CHECK(PolicyFor(EventId::kNotice).durability == EventDurability::kDurable);
  Event notice = NoticeEvent(PresentationStatus::kWarned, "· interrupted");
  CHECK(notice.id == EventId::kNotice);
  CHECK(notice.render);
  REQUIRE(notice.presentation.has_value());
  CHECK(notice.presentation->kind == PresentationKind::kNotice);
  CHECK(notice.presentation->status == PresentationStatus::kWarned);
  CHECK(notice.data["text"] == "· interrupted");
  SessionJournal notices;
  notices.Append(notice, PolicyFor(notice.id));
  CHECK(notices.Size() == 1);

  // A future GUI consumes the complete application event protocol directly;
  // streaming text is copied into the envelope and never depends on terminal
  // rendering or the public/redacted JSONL projection.
  Observability observable;
  std::vector<AppEvent> received;
  uint64_t subscription = observable.Subscribe(
      [&](const AppEvent& event) { received.push_back(event); });
  CHECK(subscription != 0);
  Event delta{EventId::kAnswerDelta};
  delta.text = "streamed";
  observable.Emit(std::move(delta));
  observable.Emit(Event{EventId::kApprovalRequested,
                        {{"id", "approval-1"}, {"tool", "edit_file"}}});
  CHECK(received.size() == 2);
  CHECK(received[0].sequence == 1);
  CHECK(received[0].type == "response.answer.delta");
  CHECK(received[0].data["text"] == "streamed");
  CHECK(received[1].type == "approval.requested");
  CHECK(received[1].data["tool"] == "edit_file");
  observable.Unsubscribe(subscription);
  observable.Emit(Event{EventId::kResponseFinished});
  CHECK(received.size() == 2);

  Observability interactions;
  interactions.EnableTerminal(false);
  std::vector<AppEvent> interaction_events;
  interactions.Subscribe(
      [&](const AppEvent& event) { interaction_events.push_back(event); });
  SetObservability(&interactions);
  InteractionRequest captured;
  SetInteractiveReadHandler(
      [&](const InteractionRequest& request, bool*) -> std::string {
        captured = request;
        return "2";
      });
  bool cancelled = false;
  bool eof = false;
  std::string choice = ReadChoiceLine(
      {.id = "session-choice",
       .kind = "session.select",
       .prompt = "resume #: ",
       .options = json::array({{{"value", "1"}, {"label", "first"}},
                               {{"value", "2"}, {"label", "second"}}})},
      cancelled, eof);
  SetInteractiveReadHandler({});
  SetObservability(nullptr);
  CHECK(choice == "2");
  CHECK(!cancelled);
  CHECK(!eof);
  CHECK(captured.id == "session-choice");
  CHECK(captured.options.size() == 2);
  CHECK(interaction_events.size() == 2);
  CHECK(interaction_events[0].type == "interaction.requested");
  CHECK(interaction_events[0].data["options"].size() == 2);
  CHECK(interaction_events[1].type == "interaction.resolved");
  CHECK(interaction_events[1].data["answer"] == "2");

  // Concurrent producers retain sequence order, and Unsubscribe is a lifetime
  // barrier rather than merely removing a callback from the next snapshot.
  Observability concurrent;
  concurrent.EnableTerminal(false);
  std::promise<void> first_entered;
  std::promise<void> release_first;
  std::shared_future<void> first_release = release_first.get_future().share();
  std::vector<uint64_t> sequences;
  std::mutex sequences_mutex;
  concurrent.Subscribe([&](const AppEvent& event) {
    {
      std::lock_guard<std::mutex> lock(sequences_mutex);
      sequences.push_back(event.sequence);
    }
    if (event.sequence == 1) {
      first_entered.set_value();
      first_release.wait();
    }
  });
  std::thread first_emit(
      [&] { concurrent.Emit(Event{EventId::kResponseStarted}); });
  first_entered.get_future().wait();
  auto second_emit = std::async(std::launch::async, [&] {
    concurrent.Emit(Event{EventId::kResponseFinished});
  });
  CHECK(second_emit.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::timeout);
  release_first.set_value();
  first_emit.join();
  second_emit.get();
  CHECK(sequences == std::vector<uint64_t>({1, 2}));

  Observability lifetime;
  lifetime.EnableTerminal(false);
  std::promise<void> callback_entered;
  std::promise<void> release_callback;
  std::shared_future<void> callback_release =
      release_callback.get_future().share();
  std::atomic<int> callback_count{0};
  uint64_t lifetime_subscription = lifetime.Subscribe([&](const AppEvent&) {
    ++callback_count;
    callback_entered.set_value();
    callback_release.wait();
  });
  std::thread emitting(
      [&] { lifetime.Emit(Event{EventId::kResponseStarted}); });
  callback_entered.get_future().wait();
  auto unsubscribing = std::async(
      std::launch::async, [&] { lifetime.Unsubscribe(lifetime_subscription); });
  CHECK(unsubscribing.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::timeout);
  release_callback.set_value();
  emitting.join();
  unsubscribing.get();
  lifetime.Emit(Event{EventId::kResponseFinished});
  CHECK(callback_count == 1);

  TestWorkspace workspace("events");
  SessionJournal projections;
  Event ready{EventId::kSessionReady,
              {{"model", "fixture-model"},
               {"toolset", "lean"},
               {"provenance",
                {{"format", 1},
                 {"build_id", "fixture-build"},
                 {"prompt_digest", "fixture-prompt"}}},
               {"effective_config", {{"api_key", "secret-must-not-leak"}}}}};
  projections.Append(ready, PolicyFor(ready.id));
  Event result{EventId::kToolResult,
               {{"turn", 1},
                {"step", 1},
                {"id", "poll"},
                {"name", "activity"},
                {"status", "ok"},
                {"issue_code", "schema.type"},
                {"issue_field", "wait_ms"},
                {"activity_operation", "poll"},
                {"no_change", true},
                {"activity_terminal", false}}};
  projections.Append(result, PolicyFor(result.id));
  std::string projection_path = (workspace.root / "projections.jsonl").string();
  std::string projection_error;
  CHECK(projections.Flush(projection_path, projection_error));
  std::ifstream projection_input(projection_path);
  std::string projection_text{std::istreambuf_iterator<char>(projection_input),
                              std::istreambuf_iterator<char>()};
  CHECK(projection_text.find("fixture-build") != std::string::npos);
  CHECK(projection_text.find("secret-must-not-leak") == std::string::npos);
  CHECK(projection_text.find("schema.type") != std::string::npos);
  CHECK(projection_text.find("activity_operation") != std::string::npos);
  CHECK(projection_text.find("no_change") != std::string::npos);

  SessionJournal journal;
  for (int64_t sequence = 1; sequence <= 600; ++sequence) {
    Event event{EventId::kTurnCompleted,
                {{"turn", sequence},
                 {"outcome", "ok"},
                 {"steps", 1},
                 {"usage", json::object()}}};
    journal.Append(event, PolicyFor(event.id));
  }
  CHECK(journal.Size() == 512);
  Event transient{EventId::kReasoningDelta};
  transient.text = "private reasoning";
  journal.Append(transient, PolicyFor(transient.id));
  CHECK(journal.Size() == 512);

  std::string path = (workspace.root / "events.jsonl").string();
  std::string error;
  CHECK(journal.Flush(path, error));
  struct stat state{};
  CHECK(stat(path.c_str(), &state) == 0);
  CHECK((state.st_mode & 0777) == 0600);
  std::ifstream input(path);
  std::string line;
  size_t lines = 0;
  while (std::getline(input, line)) {
    json record = json::parse(line, nullptr, false);
    CHECK(record.is_object());
    CHECK(JsonValue(record, "schema", "") == "uagent.session.event.v1");
    CHECK(JsonValue(record, "type", "") == "turn.completed");
    CHECK(JsonDump(record).find("private reasoning") == std::string::npos);
    ++lines;
  }
  CHECK(lines == 512);

  SessionJournal restored;
  CHECK(restored.Load(path, error));
  CHECK(restored.Size() == 512);
  Event resumed{EventId::kConfigChanged,
                {{"changed", json::array({"max_steps"})}}};
  restored.Append(resumed, PolicyFor(resumed.id));
  std::string resumed_path = (workspace.root / "resumed.jsonl").string();
  CHECK(restored.Flush(resumed_path, error));
  std::ifstream resumed_input(resumed_path);
  json last;
  while (std::getline(resumed_input, line)) {
    last = json::parse(line, nullptr, false);
  }
  CHECK(JsonValue(last, "seq", int64_t{0}) == 601);

  DebugSink debug;
  std::string debug_path = (workspace.root / "debug.jsonl").string();
  CHECK(debug.Start(debug_path));
  debug.Write("first", {{"value", 1}});
  debug.Flush();
  debug.Stop();
  std::ifstream debug_input(debug_path);
  CHECK(static_cast<bool>(std::getline(debug_input, line)));
  json debug_record = json::parse(line, nullptr, false);
  CHECK(JsonValue(debug_record, "event", "") == "first");
  CHECK(debug_record["data"]["value"] == 1);

  std::filesystem::path history = workspace.root / "history";
  std::filesystem::create_directories(history);
  std::filesystem::path owner = history / "kept.json";
  std::filesystem::path kept = history / "kept.json.events.jsonl";
  std::filesystem::path orphan = history / "orphan.json.events.jsonl";
  std::ofstream(owner) << "session";
  std::ofstream(kept) << "event";
  std::ofstream(orphan) << "event";
  PruneSessionJournalOrphans(history.string());
  CHECK(std::filesystem::exists(kept));
  CHECK(!std::filesystem::exists(orphan));
}

}  // namespace uagent
