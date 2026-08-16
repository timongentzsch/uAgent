// Copyright 2026 Timon Gentzsch

#include "include/agent.h"

#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/agent/session_store.h"
#include "include/agent/trace.h"
#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/ui/conversation.h"

namespace uagent {

Agent::Agent(Api& api, std::vector<Tool>& tools, ProcessSupervisor& processes,
             UsageAccumulator& side_usage, Approver approve,
             ToolRefresher refresh_tools,
             ProjectInstructions project_instructions,
             std::vector<Skill> skills, AdaptiveSystemState* adaptive_system)
    : api_(api),
      tools_(tools),
      processes_(processes),
      side_usage_(side_usage),
      schemas_(ToolSchemas(tools)),
      approve_(std::move(approve)),
      refresh_tools_(std::move(refresh_tools)),
      project_instructions_(std::move(project_instructions)),
      skills_(std::move(skills)),
      adaptive_system_(adaptive_system) {
  schema_chars_ = JsonDump(schemas_).size();
  if (Debug().Enabled()) {
    json names = json::array();
    for (const Tool& tool : tools_) names.push_back(tool.name);
    Debug().Write(
        "agent_init",
        {{"tools", std::move(names)},
         {"schemas", schemas_},
         {"schema_chars", schema_chars_},
         {"project_instruction_sources", project_instructions_.sources},
         {"project_instruction_chars", project_instructions_.text.size()},
         {"memory_sources", project_instructions_.memory_sources},
         {"memory_index_chars", project_instructions_.memory_index.size()},
         {"memory_always_chars", project_instructions_.memory_always.size()},
         {"project_instructions_truncated", project_instructions_.truncated}});
  }
  Reset();
}

void Agent::PrintTrace() const {
  PrintLatestTrace(conversation_.Archive(), tools_);
}

void Agent::PrintHistory() const {
  PrintConversationHistory(conversation_, tools_);
}

void Agent::Reset() {
  DebugLog("session_reset", {{"dropped_messages", conversation_.Size()},
                             {"prior_usage", UsageJson(session_usage_)}});
  if (adaptive_system_) adaptive_system_->Reset();
  applied_system_revision_ = 0;
  conversation_.Reset(BaselineMessages(), BaselineKinds());
  turn_search_trace_.Reset();
  session_usage_ = Usage{};
  api_.session_cost = 0;
  route_usage_.clear();
  logged_msgs_ = 0;
  logged_schemas_.clear();
  total_user_turns_ = 0;
  session_title_.clear();
  session_id_ = MakeSessionId();
  SetImageInputAvailable(api_.image_input);
  ++revision_;
}

json Agent::LatestToolTrace() const {
  json trace = LatestToolTraceJson(conversation_.Archive());
  if (!trace.empty()) return trace;
  return ToolTraceMessages(conversation_.Messages(),
                           MessageKindsJson(conversation_.Kinds()));
}

void Agent::RouteChanged() {
  session_id_ = MakeSessionId();
  SetImageInputAvailable(api_.image_input);
  ++revision_;
}

std::string Agent::ActiveRoute() const {
  return RouteKey(api_.base_url, api_.config.openrouter_provider,
                  api_.RequestModel(), api_.reasoning_effort);
}

void Agent::AddRouteUsage(const Usage& usage) {
  route_usage_[ActiveRoute()].Merge(usage);
}

// Every model response is billed the same way: parse the provider's usage
// block once, then charge it to the session and to the active route.
Usage Agent::AccountModelUsage(const json& reported) {
  Usage usage;
  usage.Add(reported);
  MergeSessionUsage(usage);
  AddRouteUsage(usage);
  return usage;
}

std::string Agent::FirstUserText() const {
  if (!session_title_.empty()) return session_title_;
  return conversation_.FirstUserText();
}

int64_t Agent::UserTurns() const {
  if (total_user_turns_ > 0) return total_user_turns_;
  return conversation_.UserTurns();
}

void Agent::PrintContext() const {
  PrintModelContext(
      api_.BuildChatBody(conversation_.Messages(), schemas_, session_id_));
}

bool Agent::Save(const std::string& path, std::string& error) const {
  SessionRecord record;
  record.metadata = {CanonicalCwd(), api_.RequestModel(), session_id_,
                     UserTurns(), FirstUserText()};
  record.state = {conversation_.Messages(),
                  conversation_.Kinds(),
                  conversation_.Archive(),
                  conversation_.DroppedSegments(),
                  ContextUsed(),
                  session_usage_,
                  route_usage_,
                  adaptive_system_ ? adaptive_system_->instructions : "",
                  adaptive_system_ ? adaptive_system_->revision : 0};
  SessionStoreStatus status = SessionStore::Save(path, record);
  if (!status.Ok()) {
    error = std::move(status.message);
    return false;
  }
  return true;
}

bool Agent::Load(const std::string& path, const std::string& expected_cwd,
                 std::string& error) {
  SessionLoadResult loaded = SessionStore::Load(path, expected_cwd);
  if (!loaded.status.Ok() || !loaded.record) {
    error = std::move(loaded.status.message);
    return false;
  }
  SessionRecord record = std::move(*loaded.record);
  if (!conversation_.Restore(std::move(record.state.messages),
                             std::move(record.state.message_kinds),
                             std::move(record.state.archive),
                             record.state.archive_dropped_segments)) {
    error = "session conversation state is invalid";
    return false;
  }
  if (adaptive_system_) {
    adaptive_system_->instructions = std::move(record.state.adaptive_system);
    adaptive_system_->revision = record.state.adaptive_system_revision;
  }
  RefreshBaseline();
  applied_system_revision_ = adaptive_system_ ? adaptive_system_->revision : 0;
  session_usage_ = record.state.usage;
  api_.session_cost = session_usage_.cost;
  route_usage_ = std::move(record.state.route_usage);
  session_id_ = std::move(record.metadata.session_id);
  if (session_id_.empty()) session_id_ = MakeSessionId();
  total_user_turns_ = record.metadata.turns;
  session_title_ = std::move(record.metadata.title);
  logged_msgs_ = 0;
  logged_schemas_.clear();
  turn_search_trace_.Reset();
  ++revision_;
  return true;
}

size_t Agent::RequestContextBytes(size_t schema_bytes) const {
  size_t bytes = JsonEstimatedBytes(conversation_.Messages());
  return api_.native_tools ? SaturatingAdd(bytes, schema_bytes) : bytes;
}

// Publishes the estimate the status row reads from the UI thread.
int64_t Agent::SnapshotContext(size_t schema_bytes) const {
  int64_t used = EstimatedTokens(RequestContextBytes(schema_bytes));
  context_snapshot_.store(used, std::memory_order_relaxed);
  return used;
}

int64_t Agent::ContextUsed() const { return SnapshotContext(schema_chars_); }

bool Agent::Compact(bool automatic, Usage* turn_usage) {
  if (MessageCount() < 2) {
    DebugLog("compact_skip", {{"reason", "empty"}, {"automatic", automatic}});
    printf("%s· nothing to compact%s\n", DIM(), RST());
    return false;
  }
  DebugLog("compact_start", {{"automatic", automatic},
                             {"messages", conversation_.Size()},
                             {"context_tokens", ContextUsed()}});
  printf("%s· %scompacting…%s\n", DIM(), automatic ? "auto-" : "", RST());
  size_t source_bytes = JsonEstimatedBytes(conversation_.Messages());
  size_t baseline_bytes = JsonEstimatedBytes(BaselineMessages());
  conversation_.Push(
      HarnessMessage("Summarize for a fresh context: goal, decisions, "
                     "current state, relevant paths, and next steps. Be "
                     "concise. Return only prose. Do not call or imitate "
                     "tools, emit tool markup, or continue the task."),
      MessageKind::kInternal);
  ChatResult r = Chat("compact", -1, json::array(), false);
  conversation_.PopBack();  // never archive the summarization instruction
  Usage compact_usage = AccountModelUsage(r.usage);
  if (turn_usage) turn_usage->Merge(compact_usage);
  bool invalid_summary = !ProseOnlyResponse(r) ||
                         source_bytes <= baseline_bytes ||
                         r.content.size() >= source_bytes - baseline_bytes;
  if (r.interrupted || !r.error.empty() || invalid_summary) {
    std::string outcome =
        r.interrupted
            ? "interrupted"
            : (!r.error.empty()
                   ? "error"
                   : (r.content.empty() ? "empty" : "invalid_summary"));
    DebugLog(
        "compact_end",
        {{"automatic", automatic}, {"outcome", outcome}, {"error", r.error}});
    if (!r.error.empty()) {
      printf("%s%s%s\n", RED(), TerminalSafe(r.error).c_str(), RST());
    } else {
      printf("%s· compaction rejected; context unchanged%s\n", DIM(), RST());
    }
    return false;
  }
  PruneAttachments(BaselineSize());
  ArchiveAll(automatic ? "auto_compact" : "manual_compact");
  conversation_.ResetHistory(BaselineMessages(), BaselineKinds());
  conversation_.Push(
      {{"role", "assistant"},
       {"content",
        "[model-generated context summary; non-authoritative]\nPrior "
        "context:\n" +
            r.content}},
      MessageKind::kInternal);
  ++revision_;
  DebugLog("compact_end", {{"automatic", automatic},
                           {"outcome", "ok"},
                           {"summary_chars", r.content.size()}});
  printf("\n%s· compacted%s\n", DIM(), RST());
  return true;
}

void Agent::DrainSubagentUsage() {
  std::string path = UsageLedger();
  std::string data;
  std::string error;
  if (!TakePrivateText(path, data, error)) {
    DebugLog("usage_ledger_error", {{"error", error}});
    return;
  }
  Usage spent;
  std::istringstream input(data);
  for (std::string line; std::getline(input, line);) {
    json entry = json::parse(line, nullptr, false);
    if (entry.is_object() && entry.contains("usage")) {
      Usage child = UsageFromJson(entry["usage"]);
      spent.Merge(child);
      if (entry.contains("routes") && entry["routes"].is_object()) {
        for (const auto& [route, value] : entry["routes"].items()) {
          route_usage_[route].Merge(UsageFromJson(value));
        }
      } else {
        std::string route = JsonValue(entry, "route", "delegated/unknown");
        route_usage_[route].Merge(child);
      }
    } else {
      spent.Merge(UsageFromJson(entry));
    }
  }
  side_usage_.Add(spent);
}

void Agent::MergeSideUsage(Usage& turn_usage) {
  DrainSubagentUsage();
  Usage spent = side_usage_.Take();
  for (const auto& [route, route_spent] : side_usage_.TakeRoutes()) {
    route_usage_[route].Merge(route_spent);
  }
  turn_usage.Merge(spent);
  MergeSessionUsage(spent);
}

void Agent::MergeSessionUsage(const Usage& usage) {
  session_usage_.Merge(usage);
  api_.session_cost = session_usage_.cost;
}

bool Agent::DrainBackground() {
  bool changed = false;
  // Extraction is maintenance, not a new conversation event.
  for (BackgroundCompletion& completion :
       BgTakeCompletedDetails(processes_, "memory")) {
    bool success =
        WIFEXITED(completion.status) && WEXITSTATUS(completion.status) == 0;
    MemoryEvent event;
    std::string receipt_error;
    bool receipt_exists = !completion.receipt_path.empty() &&
                          std::filesystem::exists(completion.receipt_path);
    bool has_receipt =
        receipt_exists &&
        ReadMemoryReceipt(completion.receipt_path, event, receipt_error);
    if (!success || !has_receipt) {
      event = {};
      if (!success) {
        event.action = "failed";
      } else if (receipt_exists) {
        event.action = "receipt_unavailable";
      } else {
        event.action = "no_change";
      }
      event.source_session = completion.source_id;
      event.timestamp = UtcStamp();
      event.automatic = true;
      if (!success) {
        event.preview =
            Utf8Trunc(OneLine(RedactMemorySecrets(completion.output)), 160);
      }
      std::string event_error;
      if (!WriteMemoryEvent(event, {}, event_error)) {
        DebugLog("memory_event_write_error", {{"error", event_error}});
      }
    }
    if (!completion.receipt_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(completion.receipt_path, ignored);
    }

    bool show = verbose_ || event.action == "created" ||
                event.action == "updated" || event.action == "failed" ||
                event.action == "receipt_unavailable";
    if (show) {
      bool warning =
          event.action == "failed" || event.action == "receipt_unavailable";
      const char* mark = warning ? "!" : "◇";
      if (event.action == "created" || event.action == "updated") mark = "◆";
      std::string label = event.action;
      if (event.action == "no_change") {
        label = "extraction complete · nothing saved";
      } else if (event.action == "receipt_unavailable") {
        label = "extraction complete · receipt unavailable";
      }
      printf("%s%s memory %s", warning ? RED() : DIM(), mark,
             TerminalSafe(label).c_str());
      if (!event.key.empty()) {
        printf(" · %s", TerminalSafe(event.key).c_str());
      }
      printf("%s\n", RST());
      if (!event.preview.empty()) {
        printf("%s  %s%s\n", DIM(), TerminalSafe(event.preview).c_str(), RST());
      }
    }
    DebugLog("memory_extract_finished",
             {{"activity_id", completion.activity_id},
              {"action", event.action},
              {"key", event.key},
              {"source_session", event.source_session},
              {"receipt_error", receipt_error}});
  }
  for (auto& note : BgTakeCompleted(processes_)) {
    size_t running = processes_.Count();
    printf("%s· bg job finished %s · %zu still running%s\n", DIM(),
           TerminalSafe(FirstLine(note)).c_str(), running, RST());
    conversation_.Push(HarnessMessage(std::move(note)), MessageKind::kInternal);
    changed = true;
  }
  if (DrainAttachments()) changed = true;
  if (changed) ++revision_;
  return changed;
}

bool Agent::DrainAttachments() {
  std::vector<Attachment> pending = Attachments().Take();
  if (pending.empty()) return false;
  std::string error;
  json content = AttachmentContent("[attached on request]", pending, error);
  if (error.empty()) {
    ImageFallbackResult fallback = ApplyImageFallbackToUserContent(content);
    if (!fallback.error.empty()) error = fallback.error;
    conversation_.Push({{"role", "user"}, {"content", std::move(content)}},
                       MessageKind::kAttachment);
  } else {
    conversation_.Push(HarnessMessage("[attachment failed] " + error),
                       MessageKind::kInternal);
  }
  DebugLog("attachments_added",
           {{"turn", turn_id_}, {"count", pending.size()}, {"error", error}});
  return true;
}

void Agent::ContinueAfterActivity() {
  RunTurn(
      "[harness continuation: background activity completed] Continue "
      "from the delivered result. Do not repeat completed work.",
      nullptr, /*harness_origin=*/true);
}

void Agent::ArchiveAll(const char* reason) {
  conversation_.ArchiveAll(reason, BaselineSize(), turn_id_,
                           api_.config.session_archive_bytes);
}

void Agent::RebuildToolSchemas() {
  schemas_ = ToolSchemas(tools_);
  schema_chars_ = JsonDump(schemas_).size();
  logged_schemas_.clear();
  if (!conversation_.Empty()) {
    conversation_.Set(0, SysMsg(), MessageKind::kSystem);
  }
  DebugLog("tool_registry_refreshed",
           {{"tools", tools_.size()}, {"schema_chars", schema_chars_}});
}

}  // namespace uagent
