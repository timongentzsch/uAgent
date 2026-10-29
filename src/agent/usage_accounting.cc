// Copyright 2026 Timon Gentzsch
// Usage accounting: what the model and delegated children spent, charged to
// the session, its routes and the turn that caused it.

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#include "include/agent/agent.h"
#include "include/agent/node.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/usage.h"

namespace uagent {

// Every model response is billed the same way: parse the provider's usage
// block once, then charge it to the session and to a route. Work that is not
// the conversation itself (compaction, a memory extractor's whole run) names
// its purpose in the provider's place, as titles and searches do.
Usage Agent::AccountModelUsage(const json& reported,
                               const std::string& purpose) {
  Usage usage;
  usage.Add(reported);
  MergeSessionUsage(usage);
  const std::string& tag = purpose.empty() ? usage_purpose_ : purpose;
  route_usage_[tag.empty() ? ActiveRoute()
                           : RouteKey(api_.base_url, tag, api_.model,
                                      api_.reasoning_effort)]
      .Merge(usage);
  return usage;
}

namespace {
// What the statistics dialog shows of other agents' work; a child reports
// every counter it keeps, and the rest would only fill the session file.
json SideStatistics(const json& statistics) {
  json shown = json::object();
  const json flat = FlattenNumericStatistics(statistics, "side_");
  for (const char* key :
       {"recorded_turns", "tool_calls", "model_calls", "duration_ms"}) {
    if (flat.contains(key)) shown[key] = flat[key];
  }
  return shown;
}
}  // namespace

void Agent::DrainSubagentUsage() {
  const std::string path = UsageLedger(processes_.Owner());
  std::string data;
  std::string error;
  if (!TakePrivateText(path, data, error)) {
    DebugLog("usage_ledger_error", {{"error", error}});
    return;
  }
  std::istringstream input(data);
  for (std::string line; std::getline(input, line);) {
    json entry = json::parse(line, nullptr, false);
    if (entry.is_object() && entry.contains("usage")) {
      Usage child = UsageFromJson(entry["usage"]);
      const int64_t parent_turn = JsonValue(entry, "parent_turn", int64_t{0});
      const json statistics =
          SideStatistics(JsonValue(entry, "statistics", json::object()));
      bool first = true;
      if (const json* routes = JsonObject(entry, "routes")) {
        for (const auto& [route, value] : routes->items()) {
          // Statistics describe the whole child and therefore belong on one
          // record, even when its usage spans several provider routes.
          side_usage_.Add(route, UsageFromJson(value), parent_turn,
                          first ? statistics : json::object());
          first = false;
        }
      }
      if (first) {
        side_usage_.Add(JsonValue(entry, "route", "delegated/unknown"), child,
                        parent_turn, statistics);
      }
    } else {
      side_usage_.Add(UsageFromJson(entry));
    }
  }
}

void Agent::AccountSideUsage(Usage* current_turn) {
  DrainSubagentUsage();
  const AccumulatedUsage batch = side_usage_.TakeAll();
  Usage total = batch.unassigned;
  for (const auto& [route, route_spent] : batch.routes) {
    route_usage_[route].Merge(route_spent);
  }
  // Titles and memory extraction belong to no turn; the session still counts
  // their calls.
  conversation_.AddStatistics(
      PrefixNumericStatistics(batch.unassigned_statistics, "side_"));
  for (const auto& [turn, attributed] : batch.turns) {
    total.Merge(attributed);
    const auto found = batch.turn_statistics.find(turn);
    const json statistics =
        found == batch.turn_statistics.end() ? json::object() : found->second;
    conversation_.AddStatistics(PrefixNumericStatistics(statistics, "side_"));
    if (current_turn && turn == turn_id_) {
      current_turn->Merge(attributed);
      MergeNumericStatistics(turn_side_statistics_, statistics);
    }
  }
  if (HasUsage(total) || !batch.turn_statistics.empty() ||
      !batch.unassigned_statistics.empty()) {
    MergeSessionUsage(total);
  }
  for (const auto& [turn, attributed] : batch.turns) {
    if (current_turn && turn == turn_id_) continue;
    const auto found = batch.turn_statistics.find(turn);
    UpdateTurnSideUsage(
        turn, attributed,
        found == batch.turn_statistics.end() ? json::object() : found->second);
  }
}

void Agent::UpdateTurnSideUsage(int64_t turn, const Usage& usage,
                                const json& statistics) {
  if (turn <= 0) return;
  Emit(Event{EventId::kTurnUsageAmended,
             {{"turn", turn},
              {"usage", UsageJson(usage)},
              {"statistics", statistics}}});
  for (const auto& [key, value] : conversation_.DisplayFacts().items()) {
    if (!value.is_object() || JsonValue(value, "kind", "") != "turn_summary") {
      continue;
    }
    json summary = JsonValue(value, "summary", json::object());
    if (JsonValue(summary, "turn", int64_t{0}) != turn) continue;
    Usage updated = UsageFromJson(JsonValue(summary, "usage", json::object()));
    updated.Merge(usage);
    json side = JsonValue(summary, "background_statistics", json::object());
    MergeNumericStatistics(side, statistics);
    const int64_t direct_tools =
        JsonValue(summary, "direct_tool_calls", int64_t{0});
    const int64_t direct_models =
        JsonValue(summary, "direct_model_calls", int64_t{0});
    summary["usage"] = UsageJson(updated);
    summary["usage_reported"] =
        JsonValue(summary, "usage_reported", false) || HasUsage(usage);
    summary["background_statistics"] = side;
    summary["tool_calls"] =
        direct_tools + JsonValue(side, "tool_calls", int64_t{0});
    summary["model_calls"] =
        direct_models + JsonValue(side, "model_calls", int64_t{0});
    json node = value;
    node["summary"] = std::move(summary);
    conversation_.RecordDisplay(key, node);
    ++revision_;
    Emit(Event{EventId::kMessageChanged, {{"node", EntryNode(node)}}});
    return;
  }
}

// The request path judges session budgets from these, not from the usage.
void Agent::SyncApiSessionUsage() {
  api_.session_cost = session_usage_.cost;
  api_.session_generated_tokens = session_usage_.GeneratedTokens();
}

void Agent::MergeSessionUsage(const Usage& usage) {
  session_usage_.Merge(usage);
  SyncApiSessionUsage();
  Emit(Event{
      EventId::kUsageUpdated,
      {{"usage", UsageJson(session_usage_)}, {"statistics", Statistics()}}});
}

}  // namespace uagent
