// Copyright 2026 Timon Gentzsch

// SessionCommand parse table: every worker kind maps to its enumerator with
// its fields extracted, and envelope-identity failures drop silently.
// ReceiptLog: first sight processes, identical retry replays, same id with
// different content rejects, and a full log of pending commands backpressures.

#include "include/app/session_command.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/app/asset_store.h"
#include "include/app/commands.h"
#include "include/app/session_host.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
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

void TestSessionCommandKinds() {
  const std::pair<const char*, session::SessionCommandKind> cases[] = {
      {"close", session::SessionCommandKind::kClose},
      {"interrupt", session::SessionCommandKind::kInterrupt},
      {"reply", session::SessionCommandKind::kReply},
      {"steer", session::SessionCommandKind::kSteer},
      {"recall", session::SessionCommandKind::kRecall},
      {"rename", session::SessionCommandKind::kRename},
      {"refresh", session::SessionCommandKind::kRefresh},
      {"permissions", session::SessionCommandKind::kPermissions},
      {"activity", session::SessionCommandKind::kActivity},
      {"model", session::SessionCommandKind::kModel},
      {"config", session::SessionCommandKind::kConfig},
      {"context", session::SessionCommandKind::kContext},
      {"fork", session::SessionCommandKind::kFork},
      {"prompt", session::SessionCommandKind::kPrompt},
      {"submit", session::SessionCommandKind::kSubmit},
  };
  for (const auto& [kind, want] : cases) {
    session::SessionCommand parsed;
    std::string error = "dirty";
    REQUIRE(session::ParseSessionCommand(CommandEnvelope(kind), kSession,
                                         kGeneration, parsed, error));
    CHECK(parsed.kind == want);
    CHECK(error.empty());
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
    std::string error;
    REQUIRE(session::ParseSessionCommand(command, kSession, kGeneration, parsed,
                                         error));
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
  std::string error;
  REQUIRE(session::ParseSessionCommand(command, kSession, kGeneration, parsed,
                                       error));
  CHECK(parsed.text == "hello");
  CHECK(parsed.client_request_id == "client-1");
  CHECK(parsed.interaction_id == "interaction-1");
  CHECK(parsed.title == "New title");
  CHECK(parsed.operation == "followup");
  CHECK(parsed.cancelled);
  CHECK(parsed.has_attachments);
  // Absent fields read as empty; attachments absence is observable because
  // the submit fast path depends on it.
  session::SessionCommand bare;
  REQUIRE(session::ParseSessionCommand(CommandEnvelope("submit"), kSession,
                                       kGeneration, bare, error));
  CHECK(bare.text.empty());
  CHECK(!bare.cancelled);
  CHECK(!bare.has_attachments);
}

void TestSessionCommandRejects() {
  session::SessionCommand parsed;
  std::string error;
  json wrong_session = CommandEnvelope("submit");
  wrong_session["session_id"] = "other";
  CHECK(!session::ParseSessionCommand(wrong_session, kSession, kGeneration,
                                      parsed, error));
  json wrong_generation = CommandEnvelope("submit");
  wrong_generation["generation"] = "other";
  CHECK(!session::ParseSessionCommand(wrong_generation, kSession, kGeneration,
                                      parsed, error));
  for (const std::string bad : {"", "short", "0123456789ABCDEF", "xyz-!@#"}) {
    json command = CommandEnvelope("submit");
    command["request_id"] = bad;
    CHECK(!session::ParseSessionCommand(command, kSession, kGeneration, parsed,
                                        error));
  }
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

void TestHostCommandKinds() {
  // Unknown names stay unknown.
  CHECK(session::ParseSessionCommandKind("teleport") ==
        session::SessionCommandKind::kUnknown);
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
  CHECK(forwarded == 18);
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
  AppContext context(RuntimeConfig{}, ConfigManager::Capture(false, {}),
                     Options{}, observability, nullptr);
  context.runtime.api.model = "test-model";
  context.agent = std::make_unique<Agent>(
      context.runtime.api, context.tools, context.runtime.processes,
      context.runtime.side_usage,
      [](const Tool&, const json&, int64_t) { return false; });
  std::vector<Attachment> attachments;
  std::string path;
  uint64_t revision = 0;
  AppSession session{context, attachments, path, revision};
  CHECK(CaptureStdout([&] {
          for (const auto& [command, label] :
               std::vector<std::pair<std::string, std::string>>{
                   {"/help", "/attach PATH"},
                   {"/status", "test-model"},
                   {"/cost", "no session spend"},
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
  CHECK(snapshot.value["state"]["view"]["blocks"][0]["text"] == "old");
  CHECK(host.Snapshot(id, {}).value == snapshot.value);
  // Equal-length replacement still invalidates through the file identity.
  record.state.messages[1]["content"] = "new";
  REQUIRE(SessionStore::Save(path, record).Ok());
  CHECK(host.Snapshot(id, {}).value["state"]["view"]["blocks"][0]["text"] ==
        "new");
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
  observation.EnableTerminal(false);
  RuntimeConfig config;
  config.memory_generate = false;
  AppContext context(config, ConfigManager::Capture(false, {}), Options{},
                     observation, &channel);
  context.runtime.api.model = "test-model";
  context.agent = std::make_unique<Agent>(
      context.runtime.api, context.tools, context.runtime.processes,
      context.runtime.side_usage,
      [](const Tool&, const json&, int64_t) { return false; });
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
      CHECK(settings["permissions"] == (index < 3 ? "default" : "ask"));
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
  REQUIRE(SessionStore::Save(path, record).Ok());
  SessionCatalogue catalogue;
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
  std::string error;
  REQUIRE(AtomicWriteFile(path, "corrupt", 0600, false, error));
  CHECK(!scan()[0].error.empty());
  REQUIRE(SessionStore::Save(path, record).Ok());
  CHECK(scan()[0].error.empty());
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
