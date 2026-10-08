// Copyright 2026 Timon Gentzsch
// Usage accounting: what the model and delegated children spent, charged to
// the session, its routes and the turn that caused it.

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#include "include/agent.h"
#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/usage.h"

namespace uagent {

// Every model response is billed the same way: parse the provider's usage
// block once, then charge it to the session and to the active route.
Usage Agent::AccountModelUsage(const json& reported) {
  Usage usage;
  usage.Add(reported);
  MergeSessionUsage(usage);
  route_usage_[ActiveRoute()].Merge(usage);
  return usage;
}

void Agent::DrainSubagentUsage() {
  std::string path = UsageLedger();
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
      const json statistics = FlattenNumericStatistics(
          JsonValue(entry, "statistics", json::object()), "side_");
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
  if (HasUsage(total) || !batch.turn_statistics.empty()) {
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
        JsonValue(summary, "direct_tool_calls",
                  JsonValue(summary, "tool_calls", int64_t{0}));
    const int64_t direct_models =
        JsonValue(summary, "direct_model_calls",
                  JsonValue(summary, "model_calls", int64_t{0}));
    summary["usage"] = UsageJson(updated);
    summary["usage_reported"] =
        JsonValue(summary, "usage_reported", false) || HasUsage(usage);
    summary["session_usage"] = UsageJson(session_usage_);
    summary["background_statistics"] = side;
    summary["tool_calls"] =
        direct_tools + JsonValue(side, "tool_calls", int64_t{0});
    summary["model_calls"] =
        direct_models + JsonValue(side, "model_calls", int64_t{0});
    json block = value;
    block["summary"] = std::move(summary);
    conversation_.RecordDisplay(key, block);
    ++revision_;
    Emit(Event{EventId::kMessageChanged, {{"block", std::move(block)}}});
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
