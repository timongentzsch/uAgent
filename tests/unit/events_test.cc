// Copyright 2026 Timon Gentzsch

#include "include/core/events.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/agent.h"
#include "include/app/runtime.h"
#include "include/app/self_description.h"
#include "include/cli/cli.h"
#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/limits.h"
#include "include/core/logs.h"
#include "include/core/turn_status.h"
#include "tests/unit/test_support.h"

namespace uagent {

TEST_CASE(TestTurnStatus) {
  TurnStatus projection;
  auto emit = [&](EventId id, json data = json::object()) {
    if (!data.contains("turn")) data["turn"] = 1;
    return projection.Consume(id, data);
  };
  emit(EventId::kTurnStarted);
  emit(EventId::kResponseStarted, {{"response_id", "r1"}});
  CHECK(projection.Status()["turn_status"] == "Waiting for model");
  CHECK(!projection.Consume(EventId::kResponseStarted,
                            {{"response_id", "judge"}}));
  emit(EventId::kReasoningDelta,
       {{"response_id", "r1"}, {"text", "**Checking "}});
  const uint64_t revision = projection.Revision();
  CHECK(!emit(EventId::kReasoningDelta, {{"text", "sources**"}}));
  CHECK(projection.Revision() == revision);
  emit(EventId::kReasoningDelta,
       {{"text", "\n"}, {"reasoning_kind", "summary"}});
  CHECK(projection.Status()["turn_status"] == "Thinking · Checking sources");
  emit(EventId::kToolArguments);
  CHECK(projection.Status()["phase"] == "preparing");
  json first = {{"occurrence_id", "a"},
                {"response_id", "r1"},
                {"activity", {{"status_label", "Running tests"}}}};
  json second = {
      {"occurrence_id", "b"}, {"response_id", "r1"}, {"name", "read_path"}};
  emit(EventId::kToolCall, first);
  CHECK(projection.Status()["phase"] == "preparing");
  emit(EventId::kApprovalRequested, {{"id", "same"}});
  emit(EventId::kInteractionRequested, {{"id", "same"}});
  emit(EventId::kApprovalResolved, {{"id", "same"}});
  CHECK(projection.Status()["phase"] == "decision");
  emit(EventId::kInteractionResolved, {{"id", "same"}});
  emit(EventId::kToolStarted, first);
  emit(EventId::kToolStarted, first);  // A progress label starts nothing new.
  emit(EventId::kToolStarted, second);
  CHECK(projection.Status()["turn_tool_calls"] == 2);
  const json both = "Running · Running tests · +1 tools";
  CHECK(projection.Status()["turn_status"] == both);
  emit(EventId::kToolCall, first);  // Duplicate preparation cannot un-start it.
  CHECK(projection.Status()["turn_status"] == both);
  emit(EventId::kToolResult, first);
  CHECK(projection.Status()["turn_status"] == "Running · read_path");
  emit(EventId::kToolResult, second);
  emit(EventId::kResponseRetry,
       {{"attempt", 2}, {"max_attempts", 3}, {"retry_at_ms", 123}});
  CHECK(projection.Status()["phase"] == "retrying");
  CHECK(projection.Status()["turn_status"] == "Retrying model · attempt 2/3");
  emit(EventId::kResponseStarted, {{"response_id", "r2"}});
  CHECK(!emit(EventId::kReasoningDelta,
              {{"response_id", "r1"}, {"text", "stale\n"}}));
  emit(
      EventId::kReasoningDelta,
      {{"text", std::string(kStatusLineBytes + 1, 'x') + "\nShort heading\n"}});
  CHECK(projection.Status()["turn_status"] == "Thinking · Short heading");
  // A long token stream grows neither the caption nor the status events.
  const uint64_t before = projection.Revision();
  for (int index = 0; index < 10000; ++index) {
    emit(EventId::kReasoningDelta, {{"text", "token "}});
  }
  CHECK(projection.Revision() == before);
  emit(EventId::kTurnStopped);
  CHECK(projection.Status()["phase"] == "finishing");
  CHECK(!emit(EventId::kToolStarted, first));
  emit(EventId::kTurnStarted, {{"turn", 2}});
  CHECK(projection.Status()["turn_tool_calls"] == 0);
  CHECK(!emit(EventId::kToolResult, second));
  CHECK(StatusLabel(std::string(kStatusLabelBytes - 1, 'x') + "µlong").size() ==
        kStatusLabelBytes - 1);
}

TEST_CASE(TestObservabilityEvents) {
  json provenance = BuildProvenanceJson();
  CHECK(JsonValue(provenance, "format", 0) == 1);
  CHECK(provenance["binary_version"] == kVersion);
  CHECK(provenance["prompt_digest"] == HashHex(JsonDump(PromptSurfaceJson())));
  CHECK(JsonValue(provenance, "build_id", "").size() == 16);

  // public_type is the --json-stream contract external consumers parse.
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
  CHECK(!PolicyFor(EventId::kReasoningDelta).Durable());
  // A tool the provider ran reaches subscribers and the public stream but is
  // not journalled: the session record holds only what this agent did.
  CHECK(std::string(PolicyFor(EventId::kHostedToolActivity).app_type) ==
        "response.hosted_tool");
  CHECK(std::string(PolicyFor(EventId::kHostedToolActivity).public_type) ==
        "response.hosted_tool");
  CHECK(PolicyFor(EventId::kHostedToolActivity).journal_type == nullptr);
  CHECK(!PolicyFor(EventId::kHostedToolActivity).Durable());
  CHECK(PolicyFor(EventId::kToolResult).Durable());
  CHECK(PolicyFor(EventId::kActivityCompleted).Durable());

  CHECK(std::string(PolicyFor(EventId::kNotice).journal_type) == "notice");
  CHECK(std::string(PolicyFor(EventId::kNotice).public_type) == "notice");
  CHECK(PolicyFor(EventId::kNotice).Durable());
  Event notice = NoticeEvent(PresentationStatus::kWarned, "interrupted");
  CHECK(notice.id == EventId::kNotice);
  REQUIRE(notice.presentation.has_value());
  CHECK(notice.presentation->status == PresentationStatus::kWarned);
  CHECK(notice.data["text"] == "interrupted");

  // App events carry streaming text themselves, whatever a terminal renders.
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
  SetObservability(&observable);
  {
    ResponseObservation response("waiting", {{"response_id", "r-2-4-1"},
                                             {"turn", 2},
                                             {"request", 4},
                                             {"attempt", 1}});
  }
  SetObservability(nullptr);
  REQUIRE(received.size() == 4);
  CHECK(received[2].type == "response.started");
  CHECK(received[2].data["response_id"] == "r-2-4-1");
  CHECK(received[2].data["turn"] == 2);
  CHECK(received[3].type == "response.finished");
  CHECK(received[3].data["response_id"] == "r-2-4-1");
  CHECK(received[3].data["turn"] == 2);
  CHECK(received[3].data["attempt"] == 1);
  observable.Unsubscribe(subscription);
  observable.Emit(Event{EventId::kResponseFinished});
  CHECK(received.size() == 4);

  Observability interactions;
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
                {"call_id", "provider-private"},
                {"occurrence_id", "response-1:opaque"},
                {"name", "activity"},
                {"status", "ok"},
                {"issue_code", "schema.type"},
                {"issue_field", "wait_ms"},
                {"activity_operation", "poll"},
                {"no_change", true},
                {"activity_terminal", false}}};
  projections.Append(result, PolicyFor(result.id));
  Event request{EventId::kRequestCompleted,
                {{"turn", 1},
                 {"step", 1},
                 {"context_tokens", 4321},
                 {"usage", {{"input", 17}}},
                 {"content", "answer-must-not-leak"}}};
  projections.Append(request, PolicyFor(request.id));
  projections.Append(notice, PolicyFor(notice.id));
  std::string projection_path = (workspace.root / "projections.jsonl").string();
  std::string projection_error;
  CHECK(projections.Flush(projection_path, projection_error));
  std::ifstream projection_input(projection_path);
  std::string projection_text{std::istreambuf_iterator<char>(projection_input),
                              std::istreambuf_iterator<char>()};
  CHECK(projection_text.find("fixture-build") != std::string::npos);
  CHECK(projection_text.find("request.completed") != std::string::npos);
  CHECK(projection_text.find("4321") != std::string::npos);
  CHECK(projection_text.find("answer-must-not-leak") == std::string::npos);
  CHECK(projection_text.find("secret-must-not-leak") == std::string::npos);
  CHECK(projection_text.find("schema.type") != std::string::npos);
  CHECK(projection_text.find("activity_operation") != std::string::npos);
  CHECK(projection_text.find("no_change") != std::string::npos);
  CHECK(projection_text.find("response-1:opaque") != std::string::npos);
  CHECK(projection_text.find("provider-private") == std::string::npos);
  CHECK(projection_text.find("\"type\":\"notice\"") != std::string::npos);

  // The file is the journal: every event is kept, and only events go in.
  const auto lines_of = [](const std::string& path) {
    std::ifstream input(path);
    std::vector<json> records;
    for (std::string line; std::getline(input, line);) {
      records.push_back(json::parse(line, nullptr, false));
    }
    return records;
  };
  const auto turn = [](int64_t number) {
    return Event{EventId::kTurnCompleted,
                 {{"turn", number},
                  {"outcome", "ok"},
                  {"steps", 1},
                  {"usage", json::object()}}};
  };
  SessionJournal journal;
  for (int64_t sequence = 1; sequence <= 600; ++sequence) {
    journal.Append(turn(sequence), PolicyFor(EventId::kTurnCompleted));
  }
  Event transient{EventId::kReasoningDelta};
  transient.text = "private reasoning";
  journal.Append(transient, PolicyFor(transient.id));
  std::string path = (workspace.root / "events.jsonl").string();
  std::string error;
  CHECK(journal.Flush(path, error));
  struct stat state{};
  CHECK(stat(path.c_str(), &state) == 0);
  CHECK((state.st_mode & 0777) == 0600);
  std::vector<json> records = lines_of(path);
  CHECK(records.size() == 600);
  for (const json& record : records) {
    CHECK(JsonValue(record, "schema", "") == "uagent.session.event.v1");
    CHECK(JsonValue(record, "type", "") == "turn.completed");
    CHECK(JsonDump(record).find("private reasoning") == std::string::npos);
  }
  // A save that follows no event leaves the file alone; a new event is
  // appended to it.
  CHECK(journal.Flush(path, error));
  struct stat again{};
  CHECK(stat(path.c_str(), &again) == 0 && again.st_size == state.st_size);
  journal.Append(turn(601), PolicyFor(EventId::kTurnCompleted));
  CHECK(journal.Flush(path, error));
  CHECK(stat(path.c_str(), &again) == 0 && again.st_ino == state.st_ino &&
        again.st_size > state.st_size);

  // A runtime continuing the file numbers its events after the file's, also
  // those it recorded before it knew the file.
  SessionJournal restored;
  restored.Append(turn(0), PolicyFor(EventId::kTurnCompleted));
  CHECK(restored.Load(path, error));
  restored.Append(turn(0), PolicyFor(EventId::kTurnCompleted));
  CHECK(restored.Flush(path, error));
  records = lines_of(path);
  CHECK(records.size() == 603);
  CHECK(JsonValue(records[601], "seq", int64_t{0}) == 602);
  CHECK(JsonValue(records[602], "seq", int64_t{0}) == 603);

  // A line cut short by a crash is dropped when the file is read; a corrupt
  // one elsewhere is reported, and the journal goes on.
  {
    std::ofstream torn(path, std::ios::app);
    torn << "{\"schema\":\"uagent.session.ev";
  }
  SessionJournal reread;
  CHECK(reread.Load(path, error));
  reread.Append(turn(0), PolicyFor(EventId::kTurnCompleted));
  CHECK(reread.Flush(path, error));
  records = lines_of(path);
  CHECK(records.size() == 604);
  CHECK(JsonValue(records.back(), "seq", int64_t{0}) == 604);
  {
    std::ofstream corrupt(path, std::ios::app);
    corrupt << "not json\n";
  }
  SessionJournal damaged;
  CHECK(!damaged.Load(path, error));
  CHECK(error == "dropped 1 invalid session journal line");
  error.clear();
  damaged.Append(turn(0), PolicyFor(EventId::kTurnCompleted));
  CHECK(damaged.Flush(path, error));
  CHECK(JsonValue(lines_of(path).back(), "seq", int64_t{0}) == 605);
  // The bad line went with that read, so the next one finds none.
  SessionJournal mended;
  CHECK(mended.Load(path, error) && error.empty());
  CHECK(lines_of(path).size() == 605);

  // Without a session file to take them, the oldest lines go past a bound.
  SessionJournal unsaved;
  for (size_t sequence = 0; sequence <= kJournalPendingLines; ++sequence) {
    unsaved.Append(turn(0), PolicyFor(EventId::kTurnCompleted));
  }
  const std::string unsaved_path = (workspace.root / "unsaved.jsonl").string();
  CHECK(unsaved.Flush(unsaved_path, error));
  records = lines_of(unsaved_path);
  CHECK(records.size() == kJournalPendingLines);
  CHECK(JsonValue(records.front(), "seq", int64_t{0}) == 2);

  // Past its cap the file keeps its newest half.
  SessionJournal large;
  const std::string large_path = (workspace.root / "large.jsonl").string();
  int64_t written = 0;
  while (static_cast<size_t>(written) * 100 <= kSessionJournalBytes) {
    large.Append(turn(++written), PolicyFor(EventId::kTurnCompleted));
  }
  CHECK(large.Flush(large_path, error));
  large.Append(turn(++written), PolicyFor(EventId::kTurnCompleted));
  CHECK(large.Flush(large_path, error));
  CHECK(std::filesystem::file_size(large_path) <=
        kSessionJournalBytes / 2 + 200);
  records = lines_of(large_path);
  CHECK(records.front().is_object());
  CHECK(JsonValue(records.back(), "seq", int64_t{0}) == written);
  // Turned off, nothing is recorded or written.
  large.SetEnabled(false);
  large.Append(turn(0), PolicyFor(EventId::kTurnCompleted));
  large.SetEnabled(true);
  const uintmax_t before = std::filesystem::file_size(large_path);
  CHECK(large.Flush(large_path, error));
  CHECK(std::filesystem::file_size(large_path) == before);

  DebugSink debug;
  std::string debug_path = (workspace.root / "debug.jsonl").string();
  CHECK(debug.Start(debug_path));
  debug.Write("first", {{"value", 1}});
  debug.Flush();
  debug.Stop();
  std::ifstream debug_input(debug_path);
  std::string line;
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

  CreatePrivateDirectories(RuntimeDir());
  const std::string dead = RuntimeDir() + "/dead.sock";
  const std::string live = RuntimeDir() + "/live.sock";
  const std::string unnamed = RuntimeDir() + "/unnamed.sock";
  for (const std::string& file : {dead, live, unnamed, unnamed + ".lock"}) {
    std::ofstream(file) << "";
  }
  std::ofstream(dead + ".lock") << "2147483646 gone";
  chmod((dead + ".lock").c_str(), 0600);
  FileLease running;
  CHECK(running.Acquire(live + ".lock", error, true));
  MaintainArtifacts();
  CHECK(!std::filesystem::exists(dead));
  CHECK(!std::filesystem::exists(dead + ".lock"));
  CHECK(std::filesystem::exists(live));
  CHECK(std::filesystem::exists(live + ".lock"));
  CHECK(std::filesystem::exists(unnamed));

  // A killed import's staging folder goes once it is a day old.
  const std::filesystem::path staging = UagentDir("staging");
  std::filesystem::create_directories(staging / "old/sub");
  std::filesystem::create_directories(staging / "new");
  std::ofstream(staging / "old/sub/file") << "x";
  std::filesystem::last_write_time(
      staging / "old",
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  MaintainArtifacts();
  CHECK(!std::filesystem::exists(staging / "old"));
  CHECK(std::filesystem::exists(staging / "new"));

  // The limit counts conversations: a full history keeps every one of them.
  const std::filesystem::path sessions = UagentDir(kHistoryDir);
  std::filesystem::create_directories(sessions);
  for (int64_t i = 0; i < HistoryFiles(); ++i) {
    const std::string session =
        (sessions / (std::to_string(i) + ".json")).string();
    std::ofstream(session) << "session";
    std::ofstream(session + ".lock") << "";
  }
  MaintainArtifacts();
  for (int64_t i = 0; i < HistoryFiles(); ++i) {
    CHECK(std::filesystem::exists(sessions / (std::to_string(i) + ".json")));
  }

  // The daily pass runs once a day, however many sessions start.
  const std::filesystem::path stale = staging / "stale";
  const auto make_stale = [&] {
    std::filesystem::create_directories(stale);
    std::filesystem::last_write_time(
        stale,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  };
  make_stale();
  MaintainArtifactsDaily();
  CHECK(!std::filesystem::exists(stale));
  make_stale();
  MaintainArtifactsDaily();
  CHECK(std::filesystem::exists(stale));
  std::filesystem::last_write_time(
      GlobalBase() + "/" + kMaintenanceStamp,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  MaintainArtifactsDaily();
  CHECK(!std::filesystem::exists(stale));
  const std::string loose = (sessions / "loose.json").string();
  std::ofstream(loose) << "x";
  chmod(loose.c_str(), 0644);
  MaintainArtifacts();
  struct stat tightened{};
  CHECK(stat(loose.c_str(), &tightened) == 0 &&
        (tightened.st_mode & 0777) == 0600);
}

TEST_CASE(TestDiagnosticsReachTheDayLog) {
  TestWorkspace workspace("diagnostics");
  ScopedEnv session("UAGENT_INTERNAL_SESSION_PATH",
                    (workspace.root / "s.json").string());
  const std::string trace = (workspace.root / "trace.jsonl").string();
  const std::string day =
      UagentDir(kLogsDir) + "/" + UtcStamp("%Y-%m-%d") + ".jsonl";
  const auto lines_of = [](const std::string& path) {
    std::ifstream input(path);
    std::vector<json> records;
    for (std::string line; std::getline(input, line);) {
      records.push_back(json::parse(line, nullptr, false));
    }
    return records;
  };
  {
    Observability observability;
    SetObservability(&observability);
    observability.SetRole("runtime");
    CHECK(observability.StartDebug(trace));
    // stderr goes to a file for the length of the call.
    const std::string captured = (workspace.root / "stderr").string();
    std::fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    const int file = open(captured.c_str(), O_CREAT | O_WRONLY, 0600);
    dup2(file, STDERR_FILENO);
    Diagnose(Severity::kWarning, "usage_ledger", "cannot write usage ledger",
             {{"error", "disk full"}});
    DebugLog("debug_only", {{"value", 1}});
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(file);
    std::ifstream printed(captured);
    std::string first;
    CHECK(std::getline(printed, first) && first == "cannot write usage ledger");
    CHECK(!std::getline(printed, first));
    observability.Shutdown();
    SetObservability(nullptr);
  }
  const std::vector<json> written = lines_of(day);
  REQUIRE(written.size() == 1);
  const json& line = written[0];
  CHECK(line["event"] == "usage_ledger");
  CHECK(line["pid"] == getpid());
  CHECK(line.contains("time"));
  CHECK(line["data"]["level"] == "warning");
  CHECK(line["data"]["role"] == "runtime");
  CHECK(line["data"]["session_id"] ==
        SessionIdFor((workspace.root / "s.json").string()));
  CHECK(line["data"]["text"] == "cannot write usage ledger");
  CHECK(line["data"]["data"]["error"] == "disk full");
  // The trace has both; the debug line goes nowhere else.
  const std::vector<json> traced = lines_of(trace);
  CHECK(traced.size() == 2);
  // At level off the day's file is not written; stderr still is.
  ScopedEnv off("UAGENT_LOG_LEVEL", "off");
  std::filesystem::remove(day);
  {
    Observability quiet;
    quiet.Diagnostic(Severity::kError, "web_restart", "cannot restart", {});
    quiet.Shutdown();
  }
  CHECK(!std::filesystem::exists(day));
}

TEST_CASE(TestLogsAreKeptWithinTheirSize) {
  TestWorkspace workspace("log-size");
  ScopedEnv limit("UAGENT_LOG_MB", "1");
  namespace fs = std::filesystem;
  const auto now = fs::file_time_type::clock::now();
  // Oldest first, across two folders: 4 x 400 KiB is more than 1 MiB.
  const fs::path files[] = {
      fs::path(UagentDir(kArtifactsDir)) / "http-oldest",
      fs::path(UagentDir(kMcpDir)) / "server-older.log",
      fs::path(UagentDir(kArtifactsDir)) / "exchange-newer",
      fs::path(UagentDir(kLogsDir)) / "2026-11-26.jsonl",
  };
  // What tools still use is older than all of them, and stays.
  const fs::path in_use[] = {
      fs::path(UagentDir(kActivityDir)) / "usage-s.jsonl",
      fs::path(UagentDir(kActivityDir)) / "1-2.log",
      fs::path(UagentDir(kArtifactsDir)) / "output-old",
      fs::path(UagentDir(kMcpDir)) / "image-old.png",
  };
  for (size_t i = 0; i < std::size(files); ++i) {
    std::ofstream(files[i]) << std::string(400 * 1024, 'x');
    fs::last_write_time(files[i], now - std::chrono::hours(4 - i));
    std::ofstream(in_use[i]) << std::string(400 * 1024, 'x');
    fs::last_write_time(in_use[i], now - std::chrono::hours(6));
  }
  // Sessions keep their own rules, however large.
  const fs::path journal =
      fs::path(UagentDir(kHistoryDir)) / "s.json.events.jsonl";
  std::ofstream(fs::path(UagentDir(kHistoryDir)) / "s.json") << "session";
  std::ofstream(journal) << std::string(2 * 1024 * 1024, 'x');
  fs::last_write_time(journal, now - std::chrono::hours(8));
  MaintainArtifacts();
  CHECK(!fs::exists(files[0]));
  CHECK(!fs::exists(files[1]));
  CHECK(fs::exists(files[2]));
  CHECK(fs::exists(files[3]));
  CHECK(fs::exists(journal));
  for (const fs::path& file : in_use) CHECK(fs::exists(file));
}

TEST_CASE(TestLogLevelsWriteTheirRecords) {
  struct Expected {
    LogKind kind;
    bool off, events, full, debug;
  };
  constexpr Expected kExpected[] = {
      {LogKind::kJournal, false, true, true, true},
      {LogKind::kDiagnostics, false, true, true, true},
      {LogKind::kMcpStderr, false, true, true, true},
      {LogKind::kCapture, false, false, true, true},
      {LogKind::kTrace, false, false, false, true},
  };
  for (const LogRecord& record : kLogRecords) {
    CHECK(std::ranges::find(kExpected, record.kind, &Expected::kind) !=
          std::end(kExpected));
  }
  for (const Expected& expected : kExpected) {
    for (const auto& [level, written] :
         {std::pair{"off", expected.off}, std::pair{"events", expected.events},
          std::pair{"full", expected.full},
          std::pair{"debug", expected.debug}}) {
      ScopedEnv setting("UAGENT_LOG_LEVEL", level);
      CHECK(Logs(expected.kind) == written);
    }
    ScopedEnv unknown("UAGENT_LOG_LEVEL", "loud");
    CHECK(Logs(expected.kind) == expected.full);
  }
}

}  // namespace uagent
