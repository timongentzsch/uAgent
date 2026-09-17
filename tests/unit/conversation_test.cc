// Copyright 2026 Timon Gentzsch

#include "include/ui/conversation.h"

#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/agent/session_view.h"
#include "include/agent/trace.h"
#include "include/app/runtime.h"
#include "include/core/config.h"
#include "tests/unit/terminal_test_support.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestConversation() {
  Conversation attachments;
  attachments.Push(
      {{"role", "user"},
       {"content",
        json::array({{{"type", "text"}, {"text", "inspect"}},
                     {{"type", "attachment"}, {"path", "/original.png"}}})}},
      MessageKind::kAttachment);
  CHECK(attachments.PruneAttachments(0, "vision") == 1);
  CHECK(!attachments.HasKind(MessageKind::kAttachment));
  CHECK(attachments.KindAt(0) == MessageKind::kUser);
  CHECK(attachments.At(0)["content"][1]["processed_route"] == "vision");
  attachments.PruneAttachments(attachments.Size(), "other-model");
  CHECK(attachments.At(0)["content"][1]["processed_route"] == "other-model");
  CHECK(attachments.At(0)["content"][1]["path"] == "/original.png");
  CHECK(attachments.PruneAttachments(attachments.Size(), "other-model") == 0);
  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  conversation.Push(
      {{"role", "user"}, {"content", "Prior context: this is user text"}},
      MessageKind::kUser);
  conversation.Push({{"role", "assistant"}, {"content", "[internal note]"}},
                    MessageKind::kInternal);
  conversation.Push({{"role", "system"}, {"content", "[runtime advisory]"}},
                    MessageKind::kRuntimeContext);
  conversation.Upsert(
      {{"role", "user"}, {"content", "[runtime advisory updated]"}},
      MessageKind::kRuntimeContext);
  conversation.Push({{"role", "assistant"}, {"content", "answer"}},
                    MessageKind::kAssistant);
  CHECK(conversation.FirstUserText() == "Prior context: this is user text");
  CHECK(conversation.UserTurns() == 1);
  CHECK(conversation.LastAssistantText() == "answer");
  CHECK(conversation.At(2).value("role", "") == "user");
  CHECK(conversation.LastText(MessageKind::kRuntimeContext) ==
        "[runtime advisory updated]");
  CHECK(conversation.At(3).value("role", "") == "user");
  // The wire format keeps exactly one system message, at index zero.
  json wire = conversation.Messages();
  CHECK(wire[0].value("role", "") == "system");
  for (size_t i = 1; i < wire.size(); ++i) {
    CHECK(wire[i].value("role", "") != "system");
  }

  // Set is the one mutator taking an index from the caller. Every current
  // caller rewrites message zero, which only exists once a baseline does.
  Conversation unstarted;
  unstarted.Set(0, {{"role", "system"}, {"content", "sys"}},
                MessageKind::kSystem);
  CHECK(unstarted.Empty());
  conversation.Set(conversation.Size(), {{"role", "user"}, {"content", "past"}},
                   MessageKind::kUser);
  CHECK(conversation.Size() == wire.size());
  CHECK(conversation.Kinds().size() == conversation.Size());

  // UpsertTail keeps a stable historical prefix by relocating changed runtime
  // context to the end instead of rewriting it in place.
  Conversation tailed;
  tailed.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
               {MessageKind::kSystem});
  tailed.Push({{"role", "user"}, {"content", "history"}}, MessageKind::kUser);
  tailed.UpsertTail({{"role", "user"}, {"content", "[environment: cols=80]"}},
                    MessageKind::kRuntimeContext);
  CHECK(tailed.At(1).value("content", "") == "history");
  CHECK(tailed.At(2).value("content", "") == "[environment: cols=80]");
  tailed.UpsertTail({{"role", "user"}, {"content", "[environment: cols=80]"}},
                    MessageKind::kRuntimeContext);
  CHECK(tailed.Size() == 3);
  tailed.Push({{"role", "assistant"}, {"content", "answer"}},
              MessageKind::kAssistant);
  tailed.Push({{"role", "user"}, {"content", "next turn"}}, MessageKind::kUser);
  tailed.UpsertTail({{"role", "user"}, {"content", "[environment: cols=120]"}},
                    MessageKind::kRuntimeContext);
  CHECK(tailed.LastText(MessageKind::kRuntimeContext) ==
        "[environment: cols=120]");
  CHECK(tailed.At(3).value("content", "") == "next turn");
  CHECK(tailed.At(4).value("content", "") == "[environment: cols=120]");
  for (size_t i = 1; i < tailed.Size(); ++i) {
    CHECK(tailed.At(i).value("role", "") != "system");
  }
  // UpsertTail has no caller-level LastText guard: a byte-identical entry that
  // is not at the tail is still relocated there. EnsureRuntimeContext keeps
  // the unchanged entry in place; UpsertTail's contract is only "no churn when
  // the tail already matches".
  tailed.UpsertTail({{"role", "user"}, {"content", "[environment: cols=120]"}},
                    MessageKind::kRuntimeContext);
  CHECK(tailed.Size() == 5);
  CHECK(tailed.At(4).value("content", "") == "[environment: cols=120]");
  tailed.Push({{"role", "assistant"}, {"content", "follow-up"}},
              MessageKind::kAssistant);
  tailed.UpsertTail({{"role", "user"}, {"content", "[environment: cols=120]"}},
                    MessageKind::kRuntimeContext);
  CHECK(tailed.Size() == 6);
  CHECK(tailed.At(4).value("content", "") == "follow-up");
  CHECK(tailed.At(5).value("content", "") == "[environment: cols=120]");
  MessageKind environment_kind = MessageKind::kInternal;
  CHECK(!ParseMessageKind("environment", environment_kind));

  conversation.ArchiveRange("test", 1, conversation.Size(), 1, 4096);
  CHECK(conversation.ArchivedSegments() == 1);
  CHECK(conversation.Archive()[0]["message_kinds"].size() == 4);
  CHECK(conversation.Archive()[0]["message_kinds"][0] == "user");

  Conversation bounded;
  bounded.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                {MessageKind::kSystem});
  bounded.Push({{"role", "user"}, {"content", std::string(80, 'a')}},
               MessageKind::kUser);
  bounded.ArchiveRange("first", 1, bounded.Size(), 1, 4096);
  CHECK(bounded.ArchivedSegments() == 1);
  int64_t one_segment_bytes = bounded.ArchivedBytes();
  bounded.ArchiveRange("next", 1, bounded.Size(), 2, one_segment_bytes);
  CHECK(bounded.ArchivedSegments() == 1);
  CHECK(bounded.Archive()[0]["turn"] == 2);
  CHECK(bounded.ArchivedBytes() <= one_segment_bytes);
  CHECK(bounded.DroppedSegments() == 1);
  for (int turn = 3; turn <= 6; ++turn) {
    CHECK(bounded.ArchiveRange("next", 1, bounded.Size(), turn, 4096));
  }
  Conversation restored;
  CHECK(restored.Restore(bounded.Messages(), bounded.Kinds(), bounded.Archive(),
                         bounded.DroppedSegments()));
  CHECK(restored.ArchivedBytes() ==
        static_cast<int64_t>(JsonDump(restored.Archive()).size()) - 2);
  CHECK(
      restored.ArchiveRange("next", 1, restored.Size(), 7, one_segment_bytes));
  CHECK(restored.ArchivedSegments() == 1);
  CHECK(restored.Archive()[0]["turn"] == 7);
  CHECK(restored.DroppedSegments() == 6);
  CHECK(restored.ArchivedBytes() ==
        static_cast<int64_t>(JsonDump(restored.Archive()).size()) - 2);

  Conversation rejected;
  rejected.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                 {MessageKind::kSystem});
  rejected.Push({{"role", "user"}, {"content", "payload"}}, MessageKind::kUser);
  rejected.ArchiveRange("disabled", 1, rejected.Size(), 1, 0);
  rejected.ArchiveRange("oversized", 1, rejected.Size(), 2, 1);
  CHECK(rejected.ArchivedSegments() == 0);
  CHECK(rejected.ArchivedBytes() == 0);
  CHECK(rejected.DroppedSegments() == 2);

  Conversation traces;
  traces.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
               {MessageKind::kSystem});
  auto tool_turn = [&](int turn, const std::string& name, char fill) {
    std::string id = "call-" + std::to_string(turn);
    traces.Push({{"role", "user"}, {"content", "turn " + std::to_string(turn)}},
                MessageKind::kUser);
    traces.Push({{"role", "assistant"},
                 {"content", ""},
                 {"tool_calls",
                  json::array({{{"id", id},
                                {"type", "function"},
                                {"function",
                                 {{"name", name}, {"arguments", "{}"}}}}})}},
                MessageKind::kAssistant);
    traces.Push({{"role", "tool"},
                 {"tool_call_id", id},
                 {"content", std::string(2000, fill)}},
                MessageKind::kToolResult);
  };
  tool_turn(1, "read_file", 'a');
  tool_turn(2, "skill", 'b');
  tool_turn(3, "read_file", 'c');
  tool_turn(4, "read_file", 'd');
  tool_turn(5, "read_file", 'e');
  traces.ArchiveRange("test_trace", 1, traces.Size(), 5, size_t{64} * 1024);
  ToolTracePruneResult pruned =
      traces.PruneOldToolResults(1500, 2500, {"skill"});
  CHECK(pruned.results == 2);
  CHECK(pruned.reclaimed_chars > 2500);
  CHECK(traces.At(3)
            .value("content", "")
            .starts_with("[old tool output compacted:"));
  CHECK(traces.At(6).value("content", "") == std::string(2000, 'b'));
  CHECK(traces.At(9)
            .value("content", "")
            .starts_with("[old tool output compacted:"));
  CHECK(traces.At(12).value("content", "") == std::string(2000, 'd'));
  CHECK(traces.At(15).value("content", "") == std::string(2000, 'e'));
  CHECK(traces.At(3).value("role", "") == "tool");
  CHECK(traces.HasRecentToolResult("read_file", "{}", std::string(2000, 'e')));
  CHECK(!traces.HasRecentToolResult("read_file", "{}", std::string(2000, 'a')));
  CHECK(!traces.HasRecentToolResult("grep", "{}", std::string(2000, 'e')));
  CHECK(JsonDump(traces.Archive()).find(std::string(2000, 'a')) !=
        std::string::npos);
  CHECK(traces.PruneOldToolResults(1500, 2500, {"skill"}).results == 0);

  // Prose alongside a call is transcript, not a second call protocol: an
  // assistant message with no tool_calls array contributes no trace rows.
  json prose_messages = json::array(
      {{{"role", "assistant"}, {"content", "[uagent_tool_call]{}"}},
       {{"role", "system"}, {"content", "[tool_result read_path]\nentry"}}});
  json prose_kinds = json::array({"assistant", "tool_result"});
  CHECK(ToolTraceMessages(prose_messages, prose_kinds).empty());

  json parallel_messages = json::array(
      {{{"role", "assistant"},
        {"content", ""},
        {"tool_calls",
         json::array(
             {{{"id", "one"},
               {"function", {{"name", "read_file"}, {"arguments", "{}"}}}},
              {{"id", "two"},
               {"function", {{"name", "grep"}, {"arguments", "{}"}}}}})}},
       {{"role", "tool"}, {"tool_call_id", "one"}, {"content", "first"}},
       {{"role", "tool"}, {"tool_call_id", "two"}, {"content", "second"}}});
  json parallel_kinds =
      json::array({"assistant", "tool_result", "tool_result"});
  json parallel_trace = ToolTraceMessages(parallel_messages, parallel_kinds);
  CHECK(parallel_trace.size() == 2);
  CHECK(parallel_trace[0]["name"] == "read_file");
  CHECK(parallel_trace[0]["result"] == "first");
  CHECK(parallel_trace[1]["name"] == "grep");
  CHECK(parallel_trace[1]["result"] == "second");

  Conversation small_batch;
  small_batch.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                    {MessageKind::kSystem});
  auto small_turn = [&](int turn, char fill) {
    small_batch.Push({{"role", "user"}, {"content", std::to_string(turn)}},
                     MessageKind::kUser);
    small_batch.Push({{"role", "tool"},
                      {"tool_call_id", "missing"},
                      {"content", std::string(1500, fill)}},
                     MessageKind::kToolResult);
  };
  small_turn(1, 'x');
  small_turn(2, 'y');
  small_turn(3, 'z');
  CHECK(small_batch.PruneOldToolResults(0, 2000, {}).results == 0);
  CHECK(small_batch.At(2).value("content", "") == std::string(1500, 'x'));

  Conversation snapshots;
  snapshots.Reset(json::array({{{"role", "user"}, {"content", "inspect"}}}),
                  {MessageKind::kUser});
  auto read_snapshot = [&](const char* id, const std::string& output,
                           bool complete = true) {
    snapshots.Push(
        {{"role", "assistant"},
         {"tool_calls",
          json::array({{{"id", id},
                        {"function",
                         {{"name", "read_path"},
                          {"arguments", R"({"path":"notes"})"}}}}})}},
        MessageKind::kAssistant);
    json message = {
        {"role", "tool"}, {"tool_call_id", id}, {"content", output}};
    if (complete) message[kReadRangeField] = {"notes", 1, 100};
    snapshots.Push(std::move(message), MessageKind::kToolResult);
  };
  const std::string before =
      "arbitrary display wording\n" + std::string(2000, 'a');
  const std::string after =
      "limited is just file content\n" + std::string(2000, 'b');
  read_snapshot("before", before);
  read_snapshot("after", after);
  read_snapshot("failed", "error: " + std::string(2000, 'e'), false);
  CHECK(snapshots.Restore(json::parse(JsonDump(snapshots.Messages())),
                          snapshots.Kinds(), json::array(), 0));
  CHECK(snapshots.PruneOldToolResults(0, 1024, {}).results == 0);
  CHECK(snapshots
            .PruneOldToolResults(0, 1024, {}, ToolPruneMode::kSupersededReads,
                                 512)
            .results == 0);
  CHECK(snapshots
            .PruneOldToolResults(0, 1024, {}, ToolPruneMode::kSupersededReads,
                                 16384)
            .results == 1);
  CHECK(snapshots.At(2)["content"] != before);
  CHECK(snapshots.At(4)["content"] == after);
  CHECK(snapshots.At(6)["content"] == "error: " + std::string(2000, 'e'));
  CHECK(snapshots.Archive()[0]["messages"][0]["content"] == before);
  CHECK(snapshots
            .PruneOldToolResults(0, 1024, {}, ToolPruneMode::kSupersededReads,
                                 16384)
            .results == 0);

  json kinds = MessageKindsJson(conversation.Kinds());
  std::vector<MessageKind> parsed;
  CHECK(ParseMessageKinds(kinds, conversation.Size(), parsed));
  CHECK(parsed == conversation.Kinds());
  kinds[0] = "unknown";
  CHECK(!ParseMessageKinds(kinds, conversation.Size(), parsed));

  Conversation resumed;
  resumed.Reset(
      json::array({{{"role", "system"}, {"content", "old system"}},
                   {{"role", "system"}, {"content", "old memory index"}},
                   {{"role", "user"}, {"content", "continue"}}}),
      {MessageKind::kSystem, MessageKind::kMemory, MessageKind::kUser});
  resumed.RefreshBaseline({{"role", "system"}, {"content", "new system"}});
  CHECK(resumed.Size() == 2);
  CHECK(!resumed.HasKind(MessageKind::kMemory));
  CHECK(resumed.At(1).value("content", "") == "continue");

  // A rendered receipt is kept beside the transcript, never inside it: the
  // model's copy of a tool result must not grow by what the terminal drew.
  Conversation receipts;
  receipts.Push({{"role", "user"}, {"content", "edit"}}, MessageKind::kUser);
  receipts.Push({{"role", "tool"}, {"tool_call_id", "a"}, {"content", "ok"}},
                MessageKind::kToolResult);
  receipts.RecordToolDisplay("a", "-old\n+new");
  receipts.RecordToolDisplay("b", "orphan");
  receipts.RecordToolDisplay("c", "");  // nothing drawn, nothing kept
  CHECK(receipts.ToolDisplay("a") != nullptr);
  CHECK(*receipts.ToolDisplay("a") == "-old\n+new");
  CHECK(receipts.ToolDisplay("c") == nullptr);
  CHECK(receipts.At(1).value("content", "") == "ok");
  CHECK(!receipts.At(1).contains("display"));
  // Restoring a session carries them back; one written before receipts were
  // kept simply replays without any.
  Conversation reloaded;
  CHECK(reloaded.Restore(receipts.Messages(), receipts.Kinds(), json::array(),
                         0, receipts.ToolDisplays()));
  CHECK(reloaded.ToolDisplay("a") != nullptr);
  Conversation legacy;
  CHECK(
      legacy.Restore(receipts.Messages(), receipts.Kinds(), json::array(), 0));
  CHECK(legacy.ToolDisplay("a") == nullptr);

  Conversation browser;
  browser.Push({{"role", "system"}, {"content", "private system"}},
               MessageKind::kSystem);
  std::string private_id = browser.LastDisplayId();
  browser.Push({{"role", "user"}, {"content", "visible question"}},
               MessageKind::kUser);
  std::string user_id = browser.LastDisplayId();
  browser.Push({{"role", "assistant"}, {"content", "visible answer"}},
               MessageKind::kAssistant);
  std::string answer_id = browser.LastDisplayId();
  browser.RecordDisplay(answer_id,
                        {{"reasoning", "actual supplied reasoning"}});
  browser.AddStatistics(
      {{"model_calls", 2}, {"tool_calls", 3}, {"model_ms", 123.5}});
  browser.AddStatistics({{"model_calls", 1}, {"model_ms", 10.0}});
  CHECK(browser.Statistics()["model_calls"] == 3);
  CHECK(browser.Statistics()["model_ms"] == 133.5);
  CHECK(legacy.Statistics()["complete"] == false);
  json projection = ConversationView(browser);
  CHECK(projection["blocks"][0].contains("time"));
  CHECK(projection["blocks"][1].contains("time"));
  CHECK(!browser.Messages()[1].contains("time"));
  CHECK(projection["blocks"].size() == 2);
  CHECK(JsonDump(projection).find("private system") == std::string::npos);
  CHECK(ConversationDetail(browser, private_id, 0)["text"] == "");
  CHECK(ConversationDetail(browser, user_id, 0)["text"] == "visible question");
  CHECK(JsonDump(ConversationDetail(browser, answer_id, 0))
            .find("actual supplied reasoning") == std::string::npos);
  Conversation persisted;
  CHECK(persisted.Restore(browser.Messages(), browser.Kinds(),
                          browser.Archive(), 0, browser.ToolDisplays(),
                          browser.DisplayMetadata()));
  CHECK(ConversationView(persisted) == projection);
  CHECK(persisted.Statistics() == browser.Statistics());
  json invalid_display = browser.DisplayMetadata();
  invalid_display["ids"] = {1, 1, 1};
  CHECK(!persisted.Restore(browser.Messages(), browser.Kinds(),
                           browser.Archive(), 0, browser.ToolDisplays(),
                           invalid_display));
  CHECK(ConversationView(persisted) == projection);
  CHECK(LastMessageView(browser) == projection["blocks"].back());
  const size_t model_messages = browser.Size();
  const json completed =
      browser.RecordEntry({{"text", "Command completed"}, {"activity_id", 42}});
  CHECK(browser.Size() == model_messages);
  CHECK(completed["incoming"] == 1);
  CHECK(ConversationView(browser)["blocks"].back()["kind"] == "activity");
  CHECK(persisted.Restore(browser.Messages(), browser.Kinds(),
                          browser.Archive(), 0, browser.ToolDisplays(),
                          browser.DisplayMetadata()));
  CHECK(ConversationView(persisted) == ConversationView(browser));
  const json summary = {{"turn", 1},
                        {"tool_calls", 2},
                        {"duration_ms", 1200},
                        {"usage", {{"input", 42}}}};
  browser.RecordEntry({{"kind", "turn_summary"}, {"summary", summary}});
  CHECK(browser.Size() == model_messages);
  CHECK(ConversationView(browser)["blocks"].back()["summary"] == summary);
  CHECK(persisted.Restore(browser.Messages(), browser.Kinds(),
                          browser.Archive(), 0, browser.ToolDisplays(),
                          browser.DisplayMetadata()));
  CHECK(ConversationView(persisted) == ConversationView(browser));
  json before_compaction = browser.Statistics();
  browser.ArchiveAll("compact", 1, 1, int64_t{1024} * 1024);
  browser.ResetHistory(
      json::array({{{"role", "system"}, {"content", "summary"}}}),
      {MessageKind::kSystem});
  CHECK(browser.Statistics() == before_compaction);
  CHECK(ConversationView(browser)["blocks"][0]["time"] ==
        projection["blocks"][0]["time"]);
  for (int index = 0; index < 200; ++index) {
    browser.Push({{"role", "user"}, {"content", std::string(8192, 'x')}},
                 MessageKind::kUser);
  }
  projection = ConversationView(browser);
  CHECK(projection["blocks"].size() == 64);
  CHECK(projection["more"] == true);
  CHECK(JsonDump(projection).size() < size_t{384} * 1024);
}

void TestCompactionKeepsDisplayIdentity() {
  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  conversation.Push({{"role", "user"}, {"content", "keep me"}},
                    MessageKind::kUser);
  conversation.Push({{"role", "assistant"}, {"content", "ack"}},
                    MessageKind::kAssistant);
  const uint64_t retained = conversation.DisplayIds()[1];
  conversation.ArchiveAll("compact", 1, 1, int64_t{1024} * 1024);
  conversation.ResetHistory(
      json::array({{{"role", "system"}, {"content", "sys"}}}),
      {MessageKind::kSystem});
  conversation.PushWithDisplayId({{"role", "user"}, {"content", "keep me"}},
                                 MessageKind::kUser, retained);
  // Fresh mints stay unique past the reused id.
  conversation.Push({{"role", "user"}, {"content", "new"}}, MessageKind::kUser);
  CHECK(conversation.DisplayIds().back() != retained);
  // The archived original and the re-push collapse to one transcript block.
  json view = ConversationView(conversation);
  size_t kept = 0;
  for (const json& block : view["blocks"]) {
    if (JsonValue(block, "text", "") == "keep me") ++kept;
  }
  CHECK(kept == 1);
  // Live projection obeys the same bounded page contract as saved history.
  for (int i = 0; i < 64; ++i) {
    MergeDisplayBlock(view, {{"id", "large-" + std::to_string(i)},
                             {"sequence", i + 100},
                             {"text", std::string(16384, 'x')}});
  }
  CHECK(JsonEstimatedBytes(view) < size_t{512} * 1024);
  CHECK(view["more"] == true);
  CHECK(view["blocks"].back()["id"] == "large-63");

  // Live deltas and the saved preview are one response projection. A bounded
  // checkpoint may enrich metadata, but it cannot shorten a body this client
  // already received at the same content revision.
  json state = {{"view", {{"blocks", json::array()}}}};
  const json response = {
      {"response_id", "r-7-3-1"}, {"turn", 7}, {"request", 3}, {"attempt", 1}};
  CHECK(ApplySessionEvent(state, "response.started", response));
  CHECK(ApplySessionEvent(
      state, "response.answer.delta",
      {{"response_id", "r-7-3-1"}, {"text", "complete streamed body"}}));
  MergeDisplayBlock(state["view"], {{"id", "m-99"},
                                    {"row_id", "r-7-3-1"},
                                    {"response_id", "r-7-3-1"},
                                    {"kind", "assistant"},
                                    {"text", "preview"},
                                    {"text_bytes", 7},
                                    {"truncated", true},
                                    {"content_revision", 1},
                                    {"content_complete", false},
                                    {"status", "complete"}});
  REQUIRE(state["view"]["blocks"].size() == 1);
  const json& reconciled = state["view"]["blocks"][0];
  CHECK(reconciled["id"] == "m-99");
  CHECK(reconciled["row_id"] == "r-7-3-1");
  CHECK(reconciled["text"] == "complete streamed body");
  CHECK(reconciled["text_bytes"] == 22);
  CHECK(reconciled["content_revision"] == 1);
  CHECK(reconciled["status"] == "complete");

  // A response and its tool results share response_id, but each occurrence is
  // a distinct transcript row. Checkpoint enrichment must preserve the live
  // assistant body and reasoning while adding every saved result exactly once.
  CHECK(ApplySessionEvent(
      state, "response.reasoning.delta",
      {{"response_id", "r-7-3-1"}, {"text", "visible thinking"}}));
  MergeDisplayBlock(state["view"], {{"id", "m-100"},
                                    {"response_id", "r-7-3-1"},
                                    {"occurrence_id", "r-7-3-1:call-a"},
                                    {"kind", "tool_result"},
                                    {"text", "first result"}});
  MergeDisplayBlock(state["view"], {{"id", "m-101"},
                                    {"response_id", "r-7-3-1"},
                                    {"occurrence_id", "r-7-3-1:call-b"},
                                    {"kind", "tool_result"},
                                    {"text", "second result"}});
  REQUIRE(state["view"]["blocks"].size() == 3);
  CHECK(state["view"]["blocks"][0]["kind"] == "assistant");
  CHECK(state["view"]["blocks"][0]["reasoning"] == "visible thinking");
  CHECK(state["view"]["blocks"][1]["text"] == "first result");
  CHECK(state["view"]["blocks"][2]["text"] == "second result");
  MergeDisplayBlock(state["view"], {{"id", "m-100"},
                                    {"response_id", "r-7-3-1"},
                                    {"occurrence_id", "r-7-3-1:call-a"},
                                    {"kind", "tool_result"},
                                    {"text", "first result"},
                                    {"status", "completed"}});
  REQUIRE(state["view"]["blocks"].size() == 3);
  CHECK(state["view"]["blocks"][1]["status"] == "completed");
  CHECK(state["view"]["blocks"][0]["reasoning"] == "visible thinking");

  Conversation long_answer;
  long_answer.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                    {MessageKind::kSystem});
  const std::string full_body(9000, 'a');
  const std::string full_reasoning(9000, 'r');
  long_answer.Push({{"role", "assistant"}, {"content", full_body}},
                   MessageKind::kAssistant);
  long_answer.RecordDisplay(long_answer.LastDisplayId(),
                            {{"response_id", "r-8-2-1"},
                             {"content_revision", 1},
                             {"content_complete", true},
                             {"text_bytes", full_body.size()},
                             {"reasoning", full_reasoning},
                             {"reasoning_revision", 1},
                             {"reasoning_complete", true},
                             {"reasoning_bytes", full_reasoning.size()}});
  const json preview = LastMessageView(long_answer);
  const size_t preview_text_bytes = preview["text"].get<std::string>().size();
  CHECK(preview_text_bytes <= 4096 && preview_text_bytes < full_body.size());
  CHECK(preview["text_bytes"] == preview_text_bytes);
  CHECK(preview["retained_text_bytes"] == 9000);
  CHECK(preview["content_complete"] == false);
  const size_t preview_reasoning_bytes =
      preview["reasoning"].get<std::string>().size();
  CHECK(preview_reasoning_bytes <= 4096 &&
        preview_reasoning_bytes < full_reasoning.size());
  CHECK(preview["reasoning_bytes"] == preview_reasoning_bytes);
  CHECK(preview["retained_reasoning_bytes"] == 9000);
  CHECK(preview["reasoning_complete"] == false);

  // Deltas without a response id belong to no started block. Providers that
  // omit response ids must not append one turn's streamed text onto an
  // earlier turn's completed block; the full block still arrives via
  // message.changed.
  json live = {{"view", {{"blocks", json::array()}}}};
  MergeDisplayBlock(live["view"], {{"id", "m-1"},
                                   {"sequence", 1},
                                   {"kind", "assistant"},
                                   {"text", "first answer"},
                                   {"text_bytes", 12},
                                   {"content_revision", 1},
                                   {"content_complete", true}});
  CHECK(ApplySessionEvent(live, "response.answer.delta",
                          {{"text", "second answer"}}));
  REQUIRE(live["view"]["blocks"].size() == 1);
  CHECK(JsonValue(live["view"]["blocks"][0], "text", "") == "first answer");
  CHECK(ApplySessionEvent(live, "response.reasoning.delta",
                          {{"text", "thinking"}}));
  CHECK(JsonValue(live["view"]["blocks"][0], "reasoning", "") == "");
}

void TestHistoryReplaySkipsBareHeader() {
  Conversation replay;
  replay.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                 {MessageKind::kSystem});
  replay.Push({{"role", "user"}, {"content", "hi"}},
                MessageKind::kUser);
  // A text-empty assistant turn (tool calls only): the live presenter
  // prints its mark lazily with the first text, so the replay must not
  // leave a bare mark line either.
  replay.Push({{"role", "assistant"},
                 {"content", ""},
                 {"tool_calls",
                  json::array(
                      {{{"id", "call-1"},
                        {"function",
                         {{"name", "read_file"},
                          {"arguments", "{}"}}}}})}},
                MessageKind::kAssistant);
  bool prior_unicode = g_unicode;
  g_unicode = true;
  const std::vector<Tool> no_tools;
  const std::string drawn =
      CaptureStdout([&] { PrintConversationHistory(replay, no_tools); });
  g_unicode = prior_unicode;
  CHECK(drawn.find("hi") != std::string::npos);
  CHECK(drawn.find("µ") == std::string::npos);
  CHECK(drawn.find("uagent") == std::string::npos);
  // Control: a text turn keeps its header mark.
  Conversation spoken;
  spoken.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                 {MessageKind::kSystem});
  spoken.Push({{"role", "assistant"}, {"content", "hello"}},
                MessageKind::kAssistant);
  g_unicode = true;
  const std::string voiced =
      CaptureStdout([&] { PrintConversationHistory(spoken, no_tools); });
  g_unicode = prior_unicode;
  CHECK(voiced.find("uagent") != std::string::npos);
  CHECK(voiced.find("µ") == std::string::npos);
  CHECK(voiced.find("hello") != std::string::npos);
}

void TestToolResultHealsMissingMetadata() {
  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  // Assistant call record without response metadata, as observed live.
  conversation.Push(
      {{"role", "assistant"},
       {"content", nullptr},
       {"tool_calls",
        json::array({{{"id", "call-1"},
                      {"type", "function"},
                      {"function",
                       {{"name", "run"},
                        {"arguments", "{\"command\":\"ls\"}"}}}}})}},
      MessageKind::kAssistant);
  // Receipt facts filed under the call's own detail id …
  conversation.RecordDisplay(
      "t-hash1", {{"name", "run"},
                  {"status", "success"},
                  {"duration_ms", 12.5},
                  {"call_id", "call-1"},
                  {"response_id", "r-1"},
                  {"occurrence_id", "r-1:abc"},
                  {"detail_id", "t-hash1"},
                  {"activity", {{"label", "$ ls"}}}});
  // … but the result message lost its id metadata (only Push's time).
  conversation.Push(
      {{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", "out"}},
      MessageKind::kToolResult);
  json view = ConversationView(conversation);
  const json& result = view["blocks"].back();
  CHECK(result["kind"] == "tool_result");
  CHECK(result["name"] == "run");
  CHECK(result["status"] == "success");
  CHECK(result["duration_ms"] == 12.5);
  CHECK(result["detail_id"] == "t-hash1");
  CHECK(!result.contains("receipt_missing"));
  const json& call = view["blocks"][view["blocks"].size() - 2];
  CHECK(call["tools"][0]["status"] == "success");
  // A message with neither metadata nor facts reads complete, never a
  // forever-"running" ghost.
  conversation.Push(
      {{"role", "tool"}, {"tool_call_id", "call-2"}, {"content", "out"}},
      MessageKind::kToolResult);
  json orphan_view = ConversationView(conversation);
  const json& orphan = orphan_view["blocks"].back();
  CHECK(orphan["name"] == "tool");
  CHECK(orphan["status"] == "complete");
  CHECK(orphan["receipt_missing"] == true);
}

// Delivery receipts dedupe the per-request attachment notice. Display facts
// are evictable, so the announcement record lives beside them: the first
// delivery and later delivery changes announce, repeats stay quiet.
void TestAttachmentDeliveryAnnouncements() {
  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  conversation.Push(
      {{"role", "user"},
       {"content", json::array({{{"type", "attachment"}}})}},
      MessageKind::kAttachment);
  const std::string id = conversation.LastDisplayId();
  CHECK(conversation.AnnouncedDeliveries(id) == json::array());
  CHECK(conversation.AnnouncedDeliveries("m-9999") == json::array());
  const json first = json::array(
      {{{"id", ""}, {"name", "image.png"}, {"delivery", "Image"},
        {"path", "/tmp/image.png"}}});
  conversation.RecordAnnouncedDeliveries(id, first);
  CHECK(conversation.AnnouncedDeliveries(id) == first);
  // A delivery flip (route lost vision, file fell back to a path reference)
  // is a change the request path must announce once.
  const json degraded = json::array(
      {{{"id", ""}, {"name", "image.png"}, {"delivery", "File reference"},
        {"path", "/tmp/image.png"}}});
  CHECK(degraded != first);
  conversation.RecordAnnouncedDeliveries(id, degraded);
  CHECK(conversation.AnnouncedDeliveries(id) == degraded);
  // Empty ids and non-arrays never record.
  conversation.RecordAnnouncedDeliveries("", first);
  conversation.RecordAnnouncedDeliveries("m-1", json::object());
  CHECK(conversation.AnnouncedDeliveries("") == json::array());
  CHECK(conversation.AnnouncedDeliveries("m-1") == json::array());
  // The receipt survives a session save/restore round-trip.
  Conversation reloaded;
  CHECK(reloaded.Restore(conversation.Messages(), conversation.Kinds(),
                         conversation.Archive(),
                         conversation.DroppedSegments(),
                         conversation.ToolDisplays(),
                         conversation.DisplayMetadata()));
  CHECK(reloaded.AnnouncedDeliveries(id) == degraded);
  // Sessions written before receipts existed restore without any: the next
  // request announces once, then dedupes from there.
  Conversation legacy;
  CHECK(legacy.Restore(conversation.Messages(), conversation.Kinds(),
                       json::array(), 0));
  CHECK(legacy.AnnouncedDeliveries(id) == json::array());
  // The receipt store is bounded; bulk attachment sessions cannot grow it
  // without limit.
  for (size_t seq = 0; seq < 1100; ++seq) {
    reloaded.RecordAnnouncedDeliveries("m-bulk-" + std::to_string(seq),
                                       first);
  }
  CHECK(reloaded.AnnouncedDeliveries(id) == json::array());
  CHECK(reloaded.AnnouncedDeliveries("m-bulk-1099") == first);
}

// Under display-fact pressure the eviction used to delete the oldest keys
// first -- exactly where attachment delivery receipts live -- which made the
// request path re-print their notice after every tool result. Largest-first
// eviction drops fat tool rows instead and keeps tiny control receipts.
void TestDisplayFactEvictionKeepsSmallReceipts() {
  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  conversation.Push({{"role", "user"}, {"content", "look"}},
                    MessageKind::kAttachment);
  const std::string id = conversation.LastDisplayId();
  const json receipt = json::array(
      {{{"id", ""}, {"name", "image.png"}, {"delivery", "Image"},
        {"path", "/tmp/image.png"}}});
  conversation.RecordDisplay(id, {{"deliveries", receipt}});
  // Single facts over 64 KiB never record; flood with just-under facts
  // until the 4 MiB display budget overflows several times over.
  const std::string bulk(size_t{48} * 1024, 'x');
  for (size_t i = 0; i < 120; ++i) {
    conversation.RecordDisplay("t-flood-" + std::to_string(i),
                               {{"activity", bulk}});
  }
  const json kept =
      JsonValue(JsonValue(conversation.DisplayFacts(), id.c_str(),
                          json::object()),
                "deliveries", json::array());
  CHECK(kept == receipt);
  CHECK(JsonEstimatedBytes(conversation.DisplayFacts()) <=
        size_t{4} * 1024 * 1024);
  // Count pressure with only tiny facts left still makes progress by
  // dropping the oldest key.
  Conversation crowded;
  crowded.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                {MessageKind::kSystem});
  for (size_t i = 0; i < 4100; ++i) {
    crowded.RecordDisplay("k-" + std::to_string(i), {{"n", i}});
  }
  CHECK(crowded.DisplayFacts().size() <= size_t{4096});
}

// CLI history shows the prompt without the stored "Attached:" path trailer
// plus one gallery row per recorded delivery -- the web rendering contract.
// A user literally typing the trailer without an attachment keeps it
// verbatim.
void TestAttachmentHistoryRendering() {
  CHECK(StripAttachedTrailer("plain prompt") == "plain prompt");
  CHECK(StripAttachedTrailer("look\n\nAttached:\n- path \"/tmp/a.png\"") ==
        "look");
  CHECK(StripAttachedTrailer("look\n\nAttached:\n- path \"/tmp/a.png\"\n") ==
        "look");
  CHECK(StripAttachedTrailer("look\n\nAttached:\n- path \"/tmp/a.png\" "
                             "(from tool call \"call-1\")") == "look");
  CHECK(StripAttachedTrailer("look\n\nAttached:\nnot a reference") ==
        "look\n\nAttached:\nnot a reference");
  CHECK(StripAttachedTrailer("look\n\nAttached:\n") ==
        "look\n\nAttached:\n");
  CHECK(AttachmentDeliveryRows(json::array()) == "");
  CHECK(AttachmentDeliveryRows(json::object()) == "");

  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  const std::string prompt = "why is this slow";
  const std::string path = "/tmp/image.png";
  conversation.Push(
      {{"role", "user"},
       {"content",
        json::array(
            {{{"type", "text"},
              {"text", prompt + "\n\nAttached:\n- path \"" + path +
                           "\""}},
             {{"type", "attachment"},
              {"path", path},
              {"name", "image.png"},
              {"mime", "image/png"},
              {"bytes", 12},
              {"id", ""}}})}},
      MessageKind::kAttachment);
  conversation.RecordDisplay(
      conversation.LastDisplayId(),
      {{"deliveries",
        json::array(
            {{{"id", ""}, {"name", "image.png"}, {"delivery", "Image"},
              {"path", path}}})}});
  const std::vector<Tool> no_tools;
  bool prior_unicode = g_unicode;
  g_unicode = true;
  const std::string drawn =
      CaptureStdout([&] { PrintConversationHistory(conversation, no_tools); });
  g_unicode = prior_unicode;
  CHECK(drawn.find(prompt) != std::string::npos);
  CHECK(drawn.find(path) == std::string::npos);
  // Array user content without an attachment part keeps a literal trailer:
  // the gallery gate is the attachment part, not the marker text.
  const std::string nl(1, static_cast<char>(10));
  const std::string quote(1, static_cast<char>(34));
  Conversation literal;
  literal.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                {MessageKind::kSystem});
  literal.Push(
      {{"role", "user"},
       {"content",
        json::array(
            {{{"type", "text"},
              {"text", "note" + nl + nl + "Attached:" + nl + "- path " +
                                                      quote + "/tmp/x.png" + quote}}})}},      MessageKind::kUser);
  const std::string kept =
      CaptureStdout([&] { PrintConversationHistory(literal, no_tools); });
  CHECK(kept.find("note") != std::string::npos);
  CHECK(kept.find("/tmp/x.png") != std::string::npos);
  CHECK(drawn.find("image.png \u00b7 Image") != std::string::npos);
}

}  // namespace uagent
