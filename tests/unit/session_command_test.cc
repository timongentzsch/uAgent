// Copyright 2026 Timon Gentzsch

// SessionCommand parse table: every worker kind maps to its enumerator with
// its fields extracted, and envelope-identity failures drop silently.
// ReceiptLog: first sight processes, identical retry replays, same id with
// different content rejects, and a full log of pending commands backpressures.

#include <string>

#include "include/app/session_command.h"
#include "tests/unit/test_support.h"

namespace uagent {

static const char* kSession = "session-1";
static const char* kGeneration = "gen-1";
static const char* kRequest = "0123456789abcdef";

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
      {"guide", session::SessionCommandKind::kGuide},
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
    REQUIRE(session::ParseSessionCommand(CommandEnvelope(kind), kSession, kGeneration,
                                         parsed, error));
    CHECK(parsed.kind == want);
    CHECK(error.empty());
    CHECK(parsed.request_id == kRequest);
    CHECK(std::string(SessionCommandKindName(want)) == kind);
  }
  // Missing or unrecognized kinds flow through as kUnknown so the caller
  // answers "unsupported command" with an outcome instead of dropping.
  for (const json& command :
       {CommandEnvelope(""), json{{"session_id", kSession},
                           {"generation", kGeneration},
                           {"request_id", kRequest}},
        CommandEnvelope("teleport")}) {
    session::SessionCommand parsed;
    std::string error;
    REQUIRE(session::ParseSessionCommand(command, kSession, kGeneration,
                                          parsed, error));
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
  command["budget"] = json{{"usd", 1}};
  session::SessionCommand parsed;
  std::string error;
  REQUIRE(session::ParseSessionCommand(command, kSession, kGeneration,
                                       parsed, error));
  CHECK(parsed.text == "hello");
  CHECK(parsed.client_request_id == "client-1");
  CHECK(parsed.interaction_id == "interaction-1");
  CHECK(parsed.title == "New title");
  CHECK(parsed.operation == "followup");
  CHECK(parsed.cancelled);
  CHECK(parsed.has_attachments);
  CHECK((parsed.budget == json{{"usd", 1}}));
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
  for (const std::string& bad :
       {"", "short", "0123456789ABCDEF", "xyz-!@#"}) {
    json command = CommandEnvelope("submit");
    command["request_id"] = bad;
    CHECK(!session::ParseSessionCommand(command, kSession, kGeneration,
                                        parsed, error));
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
    const std::string id = "abcdef012345678" + std::to_string(i % 10) +
                           std::to_string(i / 10);
    command["request_id"] = id;
    json previous;
    REQUIRE(log.Check(command, id, previous) ==
            session::ReceiptVerdict::kNew);
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
  // Every worker kind parses to its host namesake, including the two the
  // host never forwards.
  CHECK(session::ParseHostCommandKind("submit") ==
        session::HostCommandKind::kSubmit);
  CHECK(session::ParseHostCommandKind("close") ==
        session::HostCommandKind::kClose);
  CHECK(session::ParseHostCommandKind("guide") ==
        session::HostCommandKind::kGuide);
  CHECK(session::ParseHostCommandKind("create") ==
        session::HostCommandKind::kCreate);
  CHECK(session::ParseHostCommandKind("delete") ==
        session::HostCommandKind::kDelete);
  CHECK(session::ParseHostCommandKind("activate") ==
        session::HostCommandKind::kActivate);
  CHECK(session::ParseHostCommandKind("teleport") ==
        session::HostCommandKind::kUnknown);
  // The forward list is exactly the worker vocabulary minus the host-side
  // close flow and the worker-local guide ping. A new worker kind that is
  // not added here strands as "unsupported command"; this test fails
  // first, not a user report.
  int forwarded = 0;
  // kUnknown terminates the vocabulary: every enumerator before it is a
  // real worker kind and must appear in the forward list unless excluded.
  for (int raw = 0;
       raw < static_cast<int>(session::SessionCommandKind::kUnknown); ++raw) {
    const auto worker = static_cast<session::SessionCommandKind>(raw);
    const bool non_forwarded =
        worker == session::SessionCommandKind::kClose ||
        worker == session::SessionCommandKind::kGuide;
    CHECK(session::ForwardsToWorker(session::ParseHostCommandKind(
                session::SessionCommandKindName(worker))) == !non_forwarded);
    forwarded += !non_forwarded ? 1 : 0;
  }
  CHECK(forwarded == 14);
  CHECK(!session::ForwardsToWorker(session::HostCommandKind::kCreate));
  CHECK(!session::ForwardsToWorker(session::HostCommandKind::kDelete));
  CHECK(!session::ForwardsToWorker(session::HostCommandKind::kActivate));
  CHECK(!session::ForwardsToWorker(session::HostCommandKind::kUnknown));
}

}  // namespace uagent
