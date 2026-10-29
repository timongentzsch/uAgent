// Copyright 2026 Timon Gentzsch

// SessionCommand parse table: every worker kind maps to its enumerator with
// its fields extracted, and envelope-identity failures drop silently.
// ReceiptLog: first sight processes, identical retry replays, same id with
// different content rejects, and a full log of pending commands backpressures.

#include "include/app/session_command.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/app/asset_store.h"
#include "include/app/commands.h"
#include "include/app/schedule.h"
#include "include/app/session_host.h"
#include "include/app/transient_batcher.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
#include "tests/unit/terminal_test_support.h"
#include "tests/unit/test_support.h"

namespace uagent {

static constexpr const char* kSession = "session-1";
static constexpr const char* kGeneration = "gen-1";
static constexpr const char* kRequest = "0123456789abcdef";

static json CommandEnvelope(const std::string& kind) {
  return {{"session_id", kSession},
          {"generation", kGeneration},
          {"kind", kind},
          {"request_id", kRequest}};
}

// A completed receipt may leave the bounded lookup cache before the sender
// wakes; the sender must still receive its exact acknowledgement.
void TestOutcomeStoreReceiptLifetime() {
  for (int round = 0; round < 32; ++round) {
    session::OutcomeStore store;
    auto live = std::make_shared<session::HostSession>();
    auto disconnected = std::make_shared<session::HostSession>();
    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    live->socket = Fd(sockets[0]);
    Fd peer(sockets[1]);
    auto pending = std::async(std::launch::async, [&] {
      bool dispatched = false;
      return store.SendCommand(live, {{"kind", "refresh"}}, "000target",
                               "client", dispatched);
    });
    char buffer[4096];
    CHECK(read(peer.Get(), buffer, sizeof buffer) > 0);
    auto retry = std::async(std::launch::async, [&] {
      bool dispatched = false;
      return store.SendCommand(live, {{"kind", "refresh"}}, "000target",
                               "client", dispatched);
    });
    CHECK(read(peer.Get(), buffer, sizeof buffer) > 0);
    for (int index = 0; index < 511; ++index) {
      bool dispatched = false;
      store.SendCommand(disconnected, {}, "seed" + std::to_string(index),
                        "seed", dispatched);
    }
    json completed = {{"request_id", "000target"},
                      {"accepted", true},
                      {"result", {{"value", 42}}}};
    CHECK(store.ResolveOutcome(*live, completed));
    bool dispatched = false;
    store.SendCommand(disconnected, {}, "zznew", "new", dispatched);
    CHECK(JsonValue(store.CommandOutcome("000target", "client"), "unknown",
                    false));
    const json receipt = pending.get();
    CHECK(JsonValue(receipt, "accepted", false));
    CHECK(receipt["request_id"] == "client");
    CHECK(receipt["result"]["value"] == 42);
    CHECK(retry.get() == receipt);
  }
}

void TestScheduledPreparationDoesNotBlockHost() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("scheduled-preparation");
  fs::create_directory(workspace.root / "bin");
  const auto git = workspace.root / "bin/git";
  const auto entered = workspace.root / "entered";
  const auto release = workspace.root / "release";
  ScopedEnv path("PATH",
                 (workspace.root / "bin").string() + ":" + getenv("PATH"));
  CreatePrivateDirectories(UagentDir("scheduled"));
  // A stop that lands during either successful or failed Git preparation
  // wins over launch and over the preparation error.
  for (int status : {0, 1}) {
    fs::remove(entered);
    fs::remove(release);
    {
      std::ofstream script(git);
      script << "#!/bin/sh\ntouch '" << entered.string()
             << "'\nfor i in $(seq 1 1000); do\n[ -f '" << release.string()
             << "' ] && exit " << status << "\nsleep 0.01\ndone\nexit 1\n";
    }
    fs::permissions(git, fs::perms::owner_all);
    const json run = {
        {"id", "run"},
        {"task_id", "task"},
        {"status", "queued"},
        {"cwd", (workspace.root / "worktree").string()},
        {"session_path", (workspace.root / "scheduled.json").string()},
        {"definition",
         {{"environment", "worktree"}, {"cwd", workspace.workspace.string()}}}};
    const json store = {
        {"v", 1}, {"tasks", json::array()}, {"runs", json::array({run})}};
    std::string error;
    REQUIRE(
        AtomicWriteFile(SchedulePath(), JsonDump(store), 0600, false, error));
    session::SessionHost host("test", 4096, {},
                              (workspace.root / "host").string());
    bool recovered = false;
    auto preparing = std::async(std::launch::async,
                                [&] { return host.RunSchedules(recovered); });
    CHECK(WaitUntil([&] { return fs::exists(entered); }, BudgetMs(5000)));
    auto catalogue =
        std::async(std::launch::async, [&] { return host.Catalogue(); });
    CHECK(catalogue.wait_for(std::chrono::milliseconds(BudgetMs(250))) ==
          std::future_status::ready);
    CHECK(!ScheduleControl({{"action", "stop"}, {"key", "run"}})
               .contains("error"));
    {
      std::ofstream done(release);
    }
    CHECK(catalogue.get()["sessions"].empty());
    (void)preparing.get();
    const json after = ReadSchedules();
    CHECK(after["runs"][0]["status"] == "interrupted");
    CHECK(!fs::exists(JsonValue(run, "session_path", "")));
    CHECK(host.Catalogue()["sessions"].empty());
    // The same rule holds if stop wins the store lock immediately before a
    // terminal update, after the supervisor already read its prior state.
    json stopping = after;
    stopping["runs"][0]["status"] = "stopping";
    CHECK(AtomicWriteFile(SchedulePath(), JsonDump(stopping), 0600, false,
                          error));
    CHECK(!UpdateScheduledRun("run", status == 0 ? "completed" : "failed")
               .contains("error"));
    CHECK(ReadSchedules()["runs"][0]["status"] == "interrupted");
  }
}

void TestSessionCommandKinds() {
  const std::pair<const char*, session::SessionCommandKind> cases[] = {
      {"close", session::SessionCommandKind::kClose},
      {"interrupt", session::SessionCommandKind::kInterrupt},
      {"reply", session::SessionCommandKind::kReply},
      {"steer", session::SessionCommandKind::kSteer},
      {"recall", session::SessionCommandKind::kRecall},
      {"insert", session::SessionCommandKind::kInsert},
      {"rename", session::SessionCommandKind::kRename},
      {"refresh", session::SessionCommandKind::kRefresh},
      {"permissions", session::SessionCommandKind::kPermissions},
      {"activity", session::SessionCommandKind::kActivity},
      {"model", session::SessionCommandKind::kModel},
      {"config", session::SessionCommandKind::kConfig},
      {"context", session::SessionCommandKind::kContext},
      {"fork", session::SessionCommandKind::kFork},
      {"submit", session::SessionCommandKind::kSubmit},
  };
  for (const auto& [kind, want] : cases) {
    session::SessionCommand parsed;
    REQUIRE(session::ParseSessionCommand(CommandEnvelope(kind), kSession,
                                         kGeneration, parsed));
    CHECK(parsed.kind == want);
    CHECK(parsed.request_id == kRequest);
  }
  // Missing or unrecognized kinds flow through as kUnknown so the caller
  // answers "unsupported command" with an outcome instead of dropping.
  for (const json& command : {CommandEnvelope(""),
                              json{{"session_id", kSession},
                                   {"generation", kGeneration},
                                   {"request_id", kRequest}},
                              CommandEnvelope("teleport")}) {
    session::SessionCommand parsed;
    REQUIRE(
        session::ParseSessionCommand(command, kSession, kGeneration, parsed));
    CHECK(parsed.kind == session::SessionCommandKind::kUnknown);
  }
}

void TestSessionCommandFields() {
  json command = CommandEnvelope("submit");
  command["text"] = "hello";
  command["client_request_id"] = "client-1";
  command["interaction_id"] = "interaction-1";
  command["title"] = "New title";
  command["operation"] = "followup";
  command["cancelled"] = true;
  command["attachments"] = json::array();
  session::SessionCommand parsed;
  REQUIRE(session::ParseSessionCommand(command, kSession, kGeneration, parsed));
  CHECK(parsed.text == "hello");
  CHECK(parsed.client_request_id == "client-1");
  CHECK(parsed.interaction_id == "interaction-1");
  CHECK(parsed.title == "New title");
  CHECK(parsed.operation == "followup");
  CHECK(parsed.cancelled);
  // Absent fields read as empty.
  session::SessionCommand bare;
  REQUIRE(session::ParseSessionCommand(CommandEnvelope("submit"), kSession,
                                       kGeneration, bare));
  CHECK(bare.text.empty());
  CHECK(!bare.cancelled);
}

void TestSessionCommandRejects() {
  session::SessionCommand parsed;
  json wrong_session = CommandEnvelope("submit");
  wrong_session["session_id"] = "other";
  CHECK(!session::ParseSessionCommand(wrong_session, kSession, kGeneration,
                                      parsed));
  json wrong_generation = CommandEnvelope("submit");
  wrong_generation["generation"] = "other";
  CHECK(!session::ParseSessionCommand(wrong_generation, kSession, kGeneration,
                                      parsed));
  for (const std::string bad : {"", "short", "0123456789ABCDEF", "xyz-!@#"}) {
    json command = CommandEnvelope("submit");
    command["request_id"] = bad;
    CHECK(
        !session::ParseSessionCommand(command, kSession, kGeneration, parsed));
  }
}

// A reader catching up takes the backlog a batch at a time: Read stops at
// its byte budget and the next call continues from where the reader got to.
void TestReplayLogReadsInBatches() {
  session::ReplayLog log(MiB(1), 1024);
  for (int index = 0; index < 10; ++index) {
    log.Publish("e", "s", "g",
                {{"kind", "note"}, {"pad", std::string(90, 'x')}}, false);
  }
  session::ReplayBatch first = log.Read(0, true, log.Cursor(), 250);
  CHECK(!first.reset);
  CHECK(first.events.size() > 1 && first.events.size() < 10);
  CHECK(first.events.front().sequence == 1);
  // Kept as the event stream sends it, so no reader formats it again.
  CHECK(first.events.front().frame->starts_with(
      "id: e:1\nevent: update\ndata: {"));
  CHECK(first.events.front().frame->ends_with("}\n\n"));
  uint64_t next = first.events.back().sequence;
  size_t seen = first.events.size();
  while (next < log.Cursor()) {
    session::ReplayBatch more = log.Read(next, true, log.Cursor(), 250);
    REQUIRE(!more.events.empty());
    CHECK(more.events.front().sequence == next + 1);
    next = more.events.back().sequence;
    seen += more.events.size();
  }
  CHECK(seen == 10);
  // A watermark holds back what was published after the reader connected.
  CHECK(log.Read(0, true, 3, 1 << 20).events.size() == 3);
}

// Streamed text and usage are held back between their first frames of a
// turn; any other event sends what is held, in order, before itself.
void TestTransientBatcherHoldsStreamedText() {
  std::vector<AppEvent> sent;
  session::TransientBatcher batcher(
      [&](const AppEvent& event) { sent.push_back(event); });
  const std::string delta(AppType(EventId::kAnswerDelta));
  const std::string usage(AppType(EventId::kUsageUpdated));
  auto sent_text = [&] {
    std::string text;
    for (const AppEvent& event : sent) {
      if (event.type == delta) text += JsonValue(event.data, "text", "");
    }
    return text;
  };
  batcher.Push({0, "", std::string(AppType(EventId::kTurnStarted))});
  for (const char* text : {"a", "b", "c"}) {
    batcher.Push({0, "", delta, {{"response_id", "r"}, {"text", text}}});
  }
  CHECK(sent_text().starts_with("a"));
  batcher.Push({0, "", usage, {{"usage", 1}}});
  batcher.Push({0, "", usage, {{"usage", 2}}});
  CHECK(sent.back().type == usage && sent.back().data["usage"] == 1);
  batcher.Push({0, "", std::string(AppType(EventId::kTurnCompleted))});
  CHECK(sent_text() == "abc");
  REQUIRE(sent.size() >= 2);
  CHECK(sent[sent.size() - 2].data["usage"] == 2);
  CHECK(sent.back().type == AppType(EventId::kTurnCompleted));
}

void TestReceiptLog() {
  session::ReceiptLog log;
  json command = CommandEnvelope("submit");
  json previous;
  CHECK(log.Check(command, kRequest, previous) ==
        session::ReceiptVerdict::kNew);
  // Identical retry replays the stored outcome.
  log.Record({{"kind", "outcome"},
              {"request_id", kRequest},
              {"accepted", true},
              {"pending", false}});
  CHECK(log.Check(command, kRequest, previous) ==
        session::ReceiptVerdict::kReplay);
  CHECK(JsonValue(previous, "accepted", false));
  // Same id, different content is a collision, not a retry.
  json altered = command;
  altered["text"] = "changed";
  CHECK(log.Check(altered, kRequest, previous) ==
        session::ReceiptVerdict::kReject);
  // Non-outcome frames never touch receipts.
  log.Record({{"kind", "state"}, {"request_id", kRequest}});
  CHECK(log.Check(command, kRequest, previous) ==
        session::ReceiptVerdict::kReplay);
}

void TestReceiptLogBackpressure() {
  session::ReceiptLog log;
  // Fill the log with pending commands: no completed receipt to evict.
  for (int i = 0; i < 256; ++i) {
    json command = CommandEnvelope("submit");
    std::string id = "abcdef012345678";
    id += std::to_string(i % 10);
    id += std::to_string(i / 10);
    command["request_id"] = id;
    json previous;
    REQUIRE(log.Check(command, id, previous) == session::ReceiptVerdict::kNew);
  }
  json overflow = CommandEnvelope("submit");
  overflow["request_id"] = "ffffffffffffffff";
  json previous;
  CHECK(log.Check(overflow, "ffffffffffffffff", previous) ==
        session::ReceiptVerdict::kReject);
  // Completing one receipt makes room again.
  log.Record({{"kind", "outcome"},
              {"request_id", "abcdef01234567800"},
              {"accepted", true},
              {"pending", false}});
  CHECK(log.Check(overflow, "ffffffffffffffff", previous) ==
        session::ReceiptVerdict::kNew);
}

// A full log evicts its oldest completed receipt, whatever the ids sort as,
// so a retry of a recent command still replays.
void TestReceiptLogEvictsOldest() {
  session::ReceiptLog log;
  json previous;
  auto completed = [&](const std::string& id) {
    json command = CommandEnvelope("submit");
    command["request_id"] = id;
    CHECK(log.Check(command, id, previous) == session::ReceiptVerdict::kNew);
    log.Record({{"kind", "outcome"},
                {"request_id", id},
                {"accepted", true},
                {"pending", false}});
    return command;
  };
  for (int i = 0; i < 255; ++i) completed("f" + std::to_string(1000 + i));
  json recent = completed("0000000000000000");
  completed("e000000000000000");
  CHECK(log.Check(recent, "0000000000000000", previous) ==
        session::ReceiptVerdict::kReplay);
}

void TestHostCommandKinds() {
  // The host runs close and saved-session management itself and
  // forwards everything else; a new kind must choose one explicitly.
  int forwarded = 0;
  for (int raw = 0;
       raw < static_cast<int>(session::SessionCommandKind::kUnknown); ++raw) {
    forwarded +=
        session::ForwardsToWorker(static_cast<session::SessionCommandKind>(raw))
            ? 1
            : 0;
  }
  CHECK(forwarded == 19);
  for (auto local : {session::SessionCommandKind::kClose,
                     session::SessionCommandKind::kCreate,
                     session::SessionCommandKind::kDelete,
                     session::SessionCommandKind::kActivate,
                     session::SessionCommandKind::kUnknown}) {
    CHECK(!session::ForwardsToWorker(local));
  }
}

// Display copies of tool files (screenshots) evict their oldest instead of
// filling the session's quota, so the user's own uploads always fit.
void TestToolCopiesKeepRoomForUserFiles() {
  TestWorkspace workspace("asset-tool-copies");
  const std::string session = UagentDir("history") + "/project/web-a.json";
  session::AssetStore store;
  for (size_t i = 0; i < kMaxSessionAssets + 8; ++i) {
    CHECK(store.Store(session, "shot", "shot.png", true, true).error.empty());
  }
  size_t copies = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(session + ".assets")) {
    copies += entry.path().extension() == ".data";
  }
  CHECK(copies <= kMaxSessionAssets / 2);
  CHECK(store.Store(session, "mine", "notes.txt", true).error.empty());
}

void TestCommandReplies() {
  TestWorkspace workspace("command-replies");
  Observability observability;
  AppContext context(RuntimeConfig{}, ConfigManager::Capture({}), Options{},
                     observability, nullptr);
  context.runtime.api.model = "test-model";
  context.agent = std::make_unique<Agent>(
      context.runtime.api, context.tools, context.runtime.processes,
      context.runtime.side_usage,
      [](const Tool&, const json&, int64_t) { return std::string("denied"); });
  std::vector<Attachment> attachments;
  std::string path;
  uint64_t revision = 0;
  AppSession session{context, attachments, path, revision};
  CHECK(CaptureStdout([&] {
          for (const auto& [command, label] :
               std::vector<std::pair<std::string, std::string>>{
                   {"/help", "/attach PATH"},
                   {"/status", "test-model"},
                   {"/cost", "no spend in this conversation"},
                   {"/context", "model request"},
                   {"/tools", "tools"},
                   {"/attach", "no pending attachments"}}) {
            const auto reply =
                RunSlashCommand(session, ParseSlashCommand(command));
            CHECK(reply.output.find(label) != std::string::npos);
            CHECK(reply.result.is_object());
            if (command == "/context") {
              CHECK(reply.result.contains("effective_config"));
              CHECK(reply.result["model_request"]["model"] == "test-model");
            }
          }
          CommandReply reply;
          reply.Print("%s %d", "formatted", 42);
          CHECK(reply.output == "formatted 42");
          reply.Print("%s", std::string(KiB(80), 'x').c_str());
          reply.Print("discarded");
          CHECK(reply.output.size() == KiB(64));
        }).empty());
}

// /cost reads each route from its full usage. It once copied input, output
// and cache reads only, so a route's hit rate left cache writes out of the
// denominator: live, one route said "cache 99%" while the total said 42%.
void TestCostRoutesCountCacheWrites() {
  TestWorkspace workspace("cost-routes");
  Observability observability;
  AppContext context(RuntimeConfig{}, ConfigManager::Capture({}), Options{},
                     observability, nullptr);
  context.runtime.api.model = "test-model";
  context.agent = std::make_unique<Agent>(
      context.runtime.api, context.tools, context.runtime.processes,
      context.runtime.side_usage,
      [](const Tool&, const json&, int64_t) { return std::string("denied"); });
  SessionRecord record;
  record.metadata = {.cwd = CanonicalCwd(),
                     .model = "test",
                     .session_id = "cost",
                     .turns = 1,
                     .title = "cost"};
  record.state.messages =
      json::array({{{"role", "system"}, {"content", "sys"}}});
  record.state.message_kinds = {MessageKind::kSystem};
  Usage spent;
  spent.input = 4;
  spent.cache_read = 6000;
  spent.cache_write = 8000;
  spent.output = 200;
  record.state.usage = spent;
  record.state.route_usage["route"] = spent;
  const std::string path = (workspace.root / "cost.json").string();
  REQUIRE(SessionStore::Save(path, record).Ok());
  std::string error;
  REQUIRE(context.agent->Load(path, CanonicalCwd(), error));
  std::vector<Attachment> attachments;
  std::string file;
  uint64_t revision = 0;
  AppSession session{context, attachments, file, revision};
  CaptureStdout([&] {
    const auto reply = RunSlashCommand(session, ParseSlashCommand("/cost"));
    CHECK(reply.output.find("route · 4 in (+6k cached) (+8k cache write)") !=
          std::string::npos);
    CHECK(reply.output.find("cache 99%") == std::string::npos);
    const size_t first = reply.output.find("cache 42%");
    CHECK(first != std::string::npos);
    CHECK(reply.output.find("cache 42%", first + 1) != std::string::npos);
  });
}

void TestSavedHistoryInvalidation() {
  TestWorkspace workspace("history-cache");
  const std::string folder =
      UagentDir(kHistoryDir) + "/" + WorkspaceId(CanonicalCwd());
  CreatePrivateDirectories(folder);
  const std::string path = folder + "/history.json";
  SessionRecord record;
  record.metadata = {.cwd = CanonicalCwd(),
                     .model = "test",
                     .session_id = "history",
                     .turns = 1,
                     .title = "history"};
  record.state.messages = json::array({{{"role", "system"}, {"content", "sys"}},
                                       {{"role", "user"}, {"content", "old"}}});
  record.state.message_kinds = {MessageKind::kSystem, MessageKind::kUser};
  REQUIRE(SessionStore::Save(path, record).Ok());
  session::SessionHost host("test", 4096);
  host.RefreshCatalogue(true);
  const auto id = HashHex(path);
  const auto snapshot = host.Snapshot(id, {});
  REQUIRE(snapshot.status == 200);
  CHECK(snapshot.value["state"]["view"]["nodes"][0]["body"]["text"] == "old");
  CHECK(host.Snapshot(id, {}).value == snapshot.value);
  // Alternating saved sessions retain their own transcript and details.
  for (int index = 0; index < 3; ++index) {
    SessionRecord other = record;
    other.metadata.session_id = "other-" + std::to_string(index);
    other.state.messages[1]["content"] = other.metadata.session_id;
    REQUIRE(SessionStore::Save(
                folder + "/" + other.metadata.session_id + ".json", other)
                .Ok());
  }
  host.RefreshCatalogue(true);
  for (int pass = 0; pass < 2; ++pass) {
    for (int index = 0; index < 3; ++index) {
      const std::string name = "other-" + std::to_string(index);
      const auto other =
          host.Snapshot(HashHex(folder + "/" + name + ".json"), {});
      REQUIRE(other.status == 200);
      CHECK(other.value["state"]["view"]["nodes"][0]["body"]["text"] == name);
    }
    CHECK(host.Snapshot(id, {}).value["state"] == snapshot.value["state"]);
  }
  // Equal-length replacement still invalidates through the file identity.
  record.state.messages[1]["content"] = "new";
  REQUIRE(SessionStore::Save(path, record).Ok());
  CHECK(host.Snapshot(id, {})
            .value["state"]["view"]["nodes"][0]["body"]["text"] == "new");
  session::SnapshotQuery detail{.has_detail = true, .detail = "m-2"};
  CHECK(host.Snapshot(id, detail).value["text"] == "new");
  std::string error;
  REQUIRE(AtomicWriteFile(path, "corrupt", 0600, false, error));
  CHECK(host.Snapshot(id, {}).status == 422);
  REQUIRE(SessionStore::Save(path, record).Ok());
  CHECK(host.Snapshot(id, detail).value["text"] == "new");
  // A symlink to the cached inode must not bypass the regular-file reader.
  const std::string moved = path + ".moved";
  std::filesystem::rename(path, moved);
  std::filesystem::create_symlink(moved, path);
  CHECK(host.Snapshot(id, {}).status == 422);
  std::filesystem::remove(path);
  CHECK(host.Snapshot(id, {}).value["state"].empty());
}

void TestSessionPersistence() {
  TestWorkspace workspace("session-persistence");
  ScopedEnv session_path("UAGENT_INTERNAL_SESSION_PATH");
  const auto prior_approval = CurrentApprovalMode();
  class Channel final : public ApplicationChannel {
   public:
    std::string path;
    std::function<std::optional<ApplicationInput>()> next;
    std::function<void(const json&)> complete;
    std::optional<ApplicationInput> NextInput() override { return next(); }
    std::string SessionPath() const override { return path; }
    std::string ReadInteraction(const InteractionRequest&, bool*) override {
      return {};
    }
    void CompleteControl(const std::string&, const json& result) override {
      complete(result);
    }
  } channel;
  channel.path = (workspace.workspace / "session.json").string();
  Observability observation;
  RuntimeConfig config;
  config.memory_generate = false;
  AppContext context(config, ConfigManager::Capture({}), Options{}, observation,
                     &channel);
  context.runtime.api.model = "test-model";
  context.agent = std::make_unique<Agent>(
      context.runtime.api, context.tools, context.runtime.processes,
      context.runtime.side_usage,
      [](const Tool&, const json&, int64_t) { return std::string("denied"); });
  const std::vector<json> requests = {
      {{"kind", "tools"}},
      {{"kind", "permissions"}},
      {{"kind", "model"}, {"operation", "catalog"}},
      {{"kind", "permissions"}, {"mode", "ask"}},
      {{"kind", "tools"}, {"operation", "profile"}, {"profile", "minimal"}},
      {{"kind", "share"}},
      {{"kind", "fork"}, {"title", "child"}},
      {{"kind", "tools"}}};
  size_t sent = 0, completed = 0;
  FileStamp before;
  channel.next = [&]() -> std::optional<ApplicationInput> {
    if (sent == requests.size()) return std::nullopt;
    before = SnapshotFile(channel.path);
    CHECK(before.size > 0);
    // Even a read-only command must flush events pending at its boundary.
    observation.Emit(Event{EventId::kConfigChanged, {{"source", "pending"}}});
    if (sent == requests.size() - 1) std::filesystem::remove(channel.path);
    return ApplicationInput{.control = requests[sent++]};
  };
  channel.complete = [&](const json& result) {
    CHECK(!result.contains("error"));
    const size_t index = completed++;
    const bool changed = index == 3 || index == 4 || index == 7;
    CHECK((SnapshotFile(channel.path) != before) == changed);
    const auto loaded = SessionStore::Inspect(channel.path);
    CHECK(loaded.record.has_value());
    if (loaded.record) {
      CHECK(loaded.record->state.messages.size() == 1);
      const auto& settings =
          loaded.record->state.display["facts"]["session-settings"];
      // Only what the conversation chose is kept; the rest is inherited.
      CHECK(settings["chosen"] ==
            (index < 3 ? json::object()
                       : json::object({{"UAGENT_APPROVAL", "ask"}})));
      if (index >= 4) CHECK(settings["tools"]["profile"] == "minimal");
    }
    std::string journal, error;
    CHECK(ReadRegularFile(channel.path + ".events.jsonl", KiB(256), journal,
                          error));
    CHECK(journal.find("pending") != std::string::npos);
    if (index == 5 || index == 6) CHECK(PathExists(result.value("path", "")));
  };
  CHECK(RunApplication(context) == 0);
  CHECK(completed == requests.size());

  // A failed journal write cannot acknowledge the new revision as saved.
  std::vector<Attachment> attachments;
  uint64_t saved = context.agent->Revision();
  AppSession session{context, attachments, channel.path, saved};
  auto resumed = SessionStore::Inspect(channel.path);
  REQUIRE(resumed.record.has_value());
  resumed.record->state.messages.push_back(
      {{"role", "user"}, {"content", "edit me"}});
  resumed.record->state.message_kinds.push_back(MessageKind::kUser);
  resumed.record->state.display = json::object();
  REQUIRE(SessionStore::Save(channel.path, *resumed.record).Ok());
  std::string error;
  REQUIRE(context.agent->Load(channel.path, CanonicalCwd(), error));
  // Rewinding forks before the message and hands it back; the original
  // conversation keeps it.
  const json forked = SessionControl(session, {{"kind", "fork"}, {"turn", 1}});
  CHECK(forked.value("prompt", "") == "edit me");
  const auto rewound = SessionStore::Inspect(forked.value("path", ""));
  REQUIRE(rewound.record.has_value());
  CHECK(rewound.record->state.messages.size() == 1);
  CHECK(SessionStore::Inspect(channel.path).record->state.messages.size() == 2);
  const FileStamp checkpoint = SnapshotFile(channel.path);
  CHECK(session.Save(error));
  CHECK(SnapshotFile(channel.path) == checkpoint);

  context.agent->Rename("retry after journal failure");
  // The journal is written when it has something new.
  session.context.observability.Emit(
      Event{EventId::kConfigChanged, {{"changed", json::array({"title"})}}});
  const std::string journal = channel.path + ".events.jsonl";
  std::filesystem::remove(journal);
  std::filesystem::create_directory(journal);
  CHECK(!session.Save(error));
  CHECK(saved != context.agent->Revision());
  std::filesystem::remove(journal);
  CHECK(session.Save(error));
  CHECK(saved == context.agent->Revision());
  SetApprovalMode(prior_approval);
}

// A title is bounded in characters, as the web's form bounds it: accented
// or other non-ASCII text gets its whole length, not a quarter of it.
void TestTitlesCountCharacters() {
  std::string accented;
  for (size_t i = 0; i < kUserTitleChars; ++i) accented += "\xc3\xa9";
  CHECK(Utf8Chars(accented) == kUserTitleChars);
  CHECK(ValidSessionTitle(accented));
  CHECK(!ValidSessionTitle(accented + "x"));
  CHECK(!ValidSessionTitle("tab\tinside"));
}

void TestSessionCatalogueCache() {
  TestWorkspace workspace("catalogue-cache");
  const std::string folder = UagentDir(kHistoryDir);
  CreatePrivateDirectories(folder);
  const std::string path = folder + "/session.json";
  SessionRecord record;
  record.metadata = {.cwd = CanonicalCwd(),
                     .model = "test",
                     .session_id = "session",
                     .turns = 2,
                     .title = "old"};
  record.state.messages =
      json::array({{{"role", "system"}, {"content", "sys"}}});
  record.state.message_kinds = {MessageKind::kSystem};
  record.state.usage.cost = 0.25;
  REQUIRE(SessionStore::Save(path, record).Ok());
  SessionCatalogue catalogue;
  // Spend reads from the header, without parsing the state.
  CHECK(catalogue.List(SessionScope::kAll).at(0).cost == 0.25);
  auto scan = [&](SessionScope scope = SessionScope::kAll) {
    const auto rows = catalogue.List(scope);
    const auto fresh = ListSessions(scope);
    CHECK(rows.size() == fresh.size());
    for (size_t i = 0; i < std::min(rows.size(), fresh.size()); ++i) {
      CHECK(rows[i].path == fresh[i].path);
      CHECK(rows[i].title == fresh[i].title);
      CHECK(rows[i].cwd == fresh[i].cwd);
      CHECK(rows[i].turns == fresh[i].turns);
      CHECK(rows[i].incoming == fresh[i].incoming);
      CHECK(rows[i].cost == fresh[i].cost);
      CHECK(rows[i].bytes == fresh[i].bytes);
      CHECK(rows[i].mtime == fresh[i].mtime);
      CHECK(rows[i].error == fresh[i].error);
    }
    return rows;
  };
  REQUIRE(scan().size() == 1);
  CHECK(scan()[0].title == "old");
  const auto modified = std::filesystem::last_write_time(path);
  record.metadata.title = "new";
  REQUIRE(SessionStore::Save(path, record).Ok());
  std::filesystem::last_write_time(path, modified);
  CHECK(scan()[0].title == "new");
  record.metadata.cwd = (workspace.root / "elsewhere").string();
  REQUIRE(SessionStore::Save(folder + "/foreign.json", record).Ok());
  CHECK(scan().size() == 2);
  CHECK(scan(SessionScope::kWorkspace).size() == 1);
  CHECK(scan().size() == 2);
  // Two sessions saved within one clock tick sort either way, so the row is
  // found by its path.
  auto row = [&](const std::vector<SessionInfo>& rows) {
    const auto found = std::ranges::find(rows, path, &SessionInfo::path);
    CHECK(found != rows.end());
    return found != rows.end() ? *found : SessionInfo{};
  };
  std::string error;
  REQUIRE(AtomicWriteFile(path, "corrupt", 0600, false, error));
  CHECK(!row(scan()).error.empty());
  REQUIRE(SessionStore::Save(path, record).Ok());
  CHECK(row(scan()).error.empty());
  const std::string moved = path + ".moved";
  std::filesystem::rename(path, moved);
  std::filesystem::create_symlink(moved, path);
  CHECK(scan().size() == 1);
  std::filesystem::remove(path);
  std::filesystem::remove(folder + "/foreign.json");
  CHECK(scan().empty());
  REQUIRE(SessionStore::Save(path, record).Ok());
  CHECK(scan().size() == 1);
}

}  // namespace uagent
