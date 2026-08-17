// Copyright 2026 Timon Gentzsch

#include "include/core/events.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "tests/unit/test_support.h"

namespace uagent {

void TestObservabilityEvents() {
  CHECK(std::string(PolicyFor(EventId::kTurnStarted).public_type) ==
        "turn.started");
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

  TestWorkspace workspace("events");
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
