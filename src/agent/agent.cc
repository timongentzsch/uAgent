// Copyright 2026 Timon Gentzsch

#include "include/agent.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace uagent {

Agent::Agent(Api& api, std::vector<Tool>& tools, ProcessSupervisor& processes,
             UsageAccumulator& side_usage, Approver approve,
             ToolRefresher refresh_tools,
             ProjectInstructions project_instructions,
             std::vector<Skill> skills)
    : api_(api),
      tools_(tools),
      processes_(processes),
      side_usage_(side_usage),
      schemas_(ToolSchemas(tools)),
      approve_(std::move(approve)),
      refresh_tools_(std::move(refresh_tools)),
      project_instructions_(std::move(project_instructions)),
      skills_(std::move(skills)) {
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
         {"project_instructions_truncated", project_instructions_.truncated}});
  }
  Reset();
}

void Agent::Reset() {
  DebugLog("session_reset", {{"dropped_messages", conversation_.Size()},
                             {"prior_usage", UsageJson(session_usage_)}});
  conversation_.Reset(BaselineMessages(), BaselineKinds());
  turn_search_trace_.Reset();
  checkpoint_candidates_ = json::array();
  pending_checkpoint_ = nullptr;
  side_effects_ = json::array();
  session_usage_ = Usage{};
  api_.session_cost = 0;
  route_usage_.clear();
  context_policy_.Reset();
  logged_msgs_ = 0;
  total_user_turns_ = 0;
  session_title_.clear();
  session_id_ = MakeSessionId();
  last_checkpoint_turn_ = 0;
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
  context_policy_.SetReported(0);
  session_id_ = MakeSessionId();
  SetImageInputAvailable(api_.image_input);
  ++revision_;
}

std::string Agent::ActiveRoute() const {
  return RouteKey(api_.base_url, api_.config.openrouter_provider, api_.model,
                  api_.reasoning_effort);
}

void Agent::AddRouteUsage(const Usage& usage) {
  route_usage_[ActiveRoute()].Merge(usage);
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
  record.metadata = {CanonicalCwd(), api_.model, session_id_, UserTurns(),
                     FirstUserText()};
  record.state = {conversation_.Messages(),
                  conversation_.Kinds(),
                  conversation_.Archive(),
                  conversation_.DroppedSegments(),
                  checkpoint_candidates_,
                  pending_checkpoint_,
                  side_effects_,
                  ContextUsed(),
                  session_usage_,
                  route_usage_};
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
  RefreshBaseline();
  checkpoint_candidates_ = std::move(record.state.checkpoint_candidates);
  pending_checkpoint_ = std::move(record.state.pending_checkpoint);
  side_effects_ = std::move(record.state.side_effects);
  session_usage_ = record.state.usage;
  api_.session_cost = session_usage_.cost;
  route_usage_ = std::move(record.state.route_usage);
  session_id_ = std::move(record.metadata.session_id);
  if (session_id_.empty()) session_id_ = MakeSessionId();
  total_user_turns_ = record.metadata.turns;
  session_title_ = std::move(record.metadata.title);
  context_policy_.Reset();
  context_policy_.SetReported(record.state.context_tokens);
  logged_msgs_ = 0;
  last_checkpoint_turn_ = 0;
  turn_search_trace_.Reset();
  ++revision_;
  return true;
}

int64_t Agent::ContextUsed() const {
  int64_t used =
      context_policy_.Used(JsonEstimatedBytes(conversation_.Messages()),
                           schema_chars_, api_.native_tools);
  context_snapshot_.store(used, std::memory_order_relaxed);
  return used;
}

bool Agent::Compact(bool automatic, Usage* turn_usage, bool apply) {
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
                     "concise."),
      MessageKind::kInternal);
  ChatResult r = Chat("compact", -1, json::array());
  conversation_.PopBack();  // never archive the summarization instruction
  Usage compact_usage;
  compact_usage.Add(r.usage);
  MergeSessionUsage(compact_usage);
  AddRouteUsage(compact_usage);
  if (turn_usage) turn_usage->Merge(compact_usage);
  bool invalid_summary = r.content.empty() || !r.tool_calls.empty() ||
                         !ParseTextToolCalls(r.content).empty() ||
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
    }
    return false;
  }
  if (!apply) {
    DebugLog("compact_end", {{"automatic", automatic},
                             {"outcome", "shadow"},
                             {"summary_chars", r.content.size()}});
    printf(
        "%s· handoff checkpoint recorded (shadow mode); route and "
        "context unchanged%s\n",
        DIM(), RST());
    return true;
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
  context_policy_.SetReported(0);
  context_policy_.ResetUrgency();
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

void Agent::DrainBackground() {
  bool changed = false;
  for (auto& note : BgTakeCompletedExcept(processes_, "memory")) {
    size_t running = processes_.PendingCount() + processes_.DetachedCount();
    printf("%s· bg job finished %s · %zu still running%s\n", DIM(),
           TerminalSafe(FirstLine(note)).c_str(), running, RST());
    conversation_.Push(HarnessMessage(std::move(note)), MessageKind::kInternal);
    changed = true;
  }
  if (DrainAttachments()) changed = true;
  if (changed) ++revision_;
}

bool Agent::DrainAttachments() {
  std::vector<Attachment> pending = Attachments().Take();
  if (pending.empty()) return false;
  std::string error;
  json content = AttachmentContent("[attached on request]", pending, error);
  if (error.empty()) {
    conversation_.Push({{"role", "user"}, {"content", std::move(content)}},
                       MessageKind::kAttachment);
  } else {
    conversation_.Push(HarnessMessage("[attachment failed] " + error),
                       MessageKind::kInternal);
  }
  context_policy_.SetReported(0);
  DebugLog("attachments_added",
           {{"turn", turn_id_}, {"count", pending.size()}, {"error", error}});
  return true;
}

void Agent::Resume() {
  Turn(
      "(Continue the interrupted task from where you left off. Do not repeat "
      "completed "
      "work.)");
}

void Agent::ArchiveRange(const char* reason, size_t begin, size_t end,
                         json metadata) {
  conversation_.ArchiveRange(reason, begin, end, turn_id_,
                             api_.config.session_archive_bytes,
                             std::move(metadata));
}

void Agent::ArchiveAll(const char* reason) {
  conversation_.ArchiveAll(reason, BaselineSize(), turn_id_,
                           api_.config.session_archive_bytes);
}

void Agent::RebuildToolSchemas() {
  schemas_ = ToolSchemas(tools_);
  schema_chars_ = JsonDump(schemas_).size();
  if (!conversation_.Empty()) {
    conversation_.Set(0, SysMsg(), MessageKind::kSystem);
  }
  DebugLog("tool_registry_refreshed",
           {{"tools", tools_.size()}, {"schema_chars", schema_chars_}});
}

}  // namespace uagent
