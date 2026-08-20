// Copyright 2026 Timon Gentzsch

#include "include/ui/conversation.h"

#include <string>
#include <vector>

#include "include/agent/trace.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestConversation() {
  json image_call = {{"id", "image-1"},
                     {"function",
                      {{"name", "show_image"},
                       {"arguments", R"({"path":"plots/result.png"})"}}}};
  CHECK(ReplayableImagePath(image_call,
                            "displayed plots/result.png inline via kitty") ==
        "plots/result.png");
  image_call["function"]["arguments"] = {{"path", "plots/other.png"}};
  CHECK(ReplayableImagePath(image_call,
                            "displayed plots/other.png inline via kitty") ==
        "plots/other.png");
  CHECK(ReplayableImagePath(image_call, "error: image is missing").empty());
  CHECK(ReplayableImagePath(image_call,
                            "[old tool output compacted: image omitted]")
            .empty());
  image_call["function"]["arguments"] = "not json";
  CHECK(ReplayableImagePath(image_call, "displayed image inline via kitty")
            .empty());
  image_call["function"]["name"] = "read_file";
  image_call["function"]["arguments"] = {{"path", "plots/result.png"}};
  CHECK(ReplayableImagePath(image_call,
                            "displayed plots/result.png inline via kitty")
            .empty());

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

  std::string text_call = std::string(kTtOpen) +
                          R"({"name":"read_path","arguments":{"path":"."}})" +
                          kTtClose;
  json text_messages = json::array(
      {{{"role", "assistant"}, {"content", text_call}},
       {{"role", "system"}, {"content", "[tool_result read_path]\nentry"}}});
  json text_kinds = json::array({"assistant", "tool_result"});
  json text_trace = ToolTraceMessages(text_messages, text_kinds);
  CHECK(text_trace.size() == 1);
  CHECK(text_trace[0]["name"] == "read_path");
  CHECK(text_trace[0]["arguments"] == json({{"path", "."}}));
  CHECK(text_trace[0]["result"] == "entry");
  CHECK(text_trace[0].value("text_protocol", false));

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
}

}  // namespace uagent
