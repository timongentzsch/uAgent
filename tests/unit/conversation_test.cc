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
  conversation.Push(
      {{"role", "user"}, {"content", "[environment: date old; cwd /old]"}},
      MessageKind::kEnvironment);
  conversation.Push(
      {{"role", "user"}, {"content", "[environment: date today; cwd /work]"}},
      MessageKind::kEnvironment);
  conversation.Push({{"role", "assistant"}, {"content", "answer"}},
                    MessageKind::kAssistant);
  CHECK(conversation.FirstUserText() == "Prior context: this is user text");
  CHECK(conversation.UserTurns() == 1);
  CHECK(conversation.LastAssistantText() == "answer");
  CHECK(conversation.LastText(MessageKind::kEnvironment) ==
        "[environment: date today; cwd /work]");
  CHECK(std::string(MessageKindName(MessageKind::kEnvironment)) ==
        "environment");
  MessageKind environment_kind = MessageKind::kInternal;
  CHECK(ParseMessageKind("environment", environment_kind));
  CHECK(environment_kind == MessageKind::kEnvironment);

  conversation.ArchiveRange("test", 1, conversation.Size(), 1, 4096);
  CHECK(conversation.ArchivedSegments() == 1);
  CHECK(conversation.Archive()[0]["message_kinds"].size() == 5);
  CHECK(conversation.Archive()[0]["message_kinds"][0] == "user");

  json kinds = MessageKindsJson(conversation.Kinds());
  std::vector<MessageKind> parsed;
  CHECK(ParseMessageKinds(kinds, conversation.Size(), parsed));
  CHECK(parsed == conversation.Kinds());
  kinds[0] = "unknown";
  CHECK(!ParseMessageKinds(kinds, conversation.Size(), parsed));

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
