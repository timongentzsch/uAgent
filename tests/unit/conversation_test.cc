// Copyright 2026 Timon Gentzsch

#include <string>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

void TestConversationAndContextPolicy() {
  Conversation conversation;
  conversation.Reset(json::array({{{"role", "system"}, {"content", "sys"}}}),
                     {MessageKind::kSystem});
  conversation.Push(
      {{"role", "user"}, {"content", "Prior context: this is user text"}},
      MessageKind::kUser);
  conversation.Push(
      {{"role", "assistant"}, {"content", "[checkpoint internal]"}},
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
  CHECK(conversation.At(2).value("role", "") == "system");
  CHECK(conversation.LastText(MessageKind::kRuntimeContext) ==
        "[runtime advisory updated]");
  CHECK(conversation.At(3).value("role", "") == "system");
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
  traces.ArchiveRange("test_trace", 1, traces.Size(), 5, 64 * 1024);
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
  resumed.RefreshBaseline({{"role", "system"}, {"content", "new system"}},
                          nullptr, nullptr);
  CHECK(resumed.Size() == 2);
  CHECK(!resumed.HasKind(MessageKind::kMemory));
  CHECK(resumed.At(1).value("content", "") == "continue");

  ContextPolicy policy;
  policy.SetReported(80);
  ContextPolicyInput input{
      .message_bytes = 100,
      .message_count = 4,
      .context_window = 100,
      .checkpoint_pct = 70,
      .urgent_pct = 80,
      .checkpoint_enabled = true,
      .turn = 1,
  };
  ContextDecision first = policy.Prepare(input);
  CHECK(first.action == ContextAction::kUrgentCheckpoint);
  CHECK(first.projected_pct == 80);
  policy.HintIssued(1);
  input.turn = 2;
  CHECK(policy.Prepare(input).action == ContextAction::kNone);
  input.turn = 4;
  CHECK(policy.Prepare(input).action == ContextAction::kUrgentCheckpoint);
  policy.HintIssued(4);
  input.turn = 7;
  ContextDecision forced = policy.Prepare(input);
  CHECK(forced.action == ContextAction::kCompact);
  CHECK(forced.forced);

  ContextPolicy growing;
  growing.SetReported(100);
  CHECK(growing.Used(/*message_bytes=*/800, /*schema_bytes=*/0,
                     /*native_tools=*/false) == 200);
  CHECK(growing.Used(/*message_bytes=*/400, /*schema_bytes=*/800,
                     /*native_tools=*/true) == 300);
}

}  // namespace uagent
