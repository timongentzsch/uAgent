// Copyright 2026 Timon Gentzsch
#include <string>
#include <utility>

#include "include/app/execution.h"
namespace uagent::execution {
json EncodeResult(const ToolResult& r) {
  json v = {{"status", static_cast<int>(r.status)},
            {"error", static_cast<int>(r.error)},
            {"output", r.output},
            {"hint", r.hint},
            {"body_kind", r.body_kind},
            {"result_chars", r.result_chars},
            {"diff", r.diff},
            {"diff_path", r.diff_path},
            {"parts", r.parts},
            {"todo", r.todo},
            {"opened_skill", r.opened_skill},
            {"recovery_key", r.recovery_key},
            {"activity_terminal", r.activity_terminal},
            {"background", r.background},
            {"ended", json::array()}};
  json facts = {{"unit", r.facts.unit},
                {"outcome", r.facts.outcome},
                {"reason", r.facts.reason},
                {"no_change", r.facts.no_change}};
  if (r.facts.exit) {
    facts["exit"] = {{"code", r.facts.exit->code},
                     {"signal", r.facts.exit->signal}};
  }
  if (r.facts.added) facts["added"] = *r.facts.added;
  if (r.facts.removed) facts["removed"] = *r.facts.removed;
  if (r.facts.items) facts["items"] = *r.facts.items;
  v["facts"] = std::move(facts);
  if (r.artifact) {
    v["artifact"] = {{"path", r.artifact->path}, {"bytes", r.artifact->bytes}};
  }
  if (r.read_range) {
    v["read_range"] = {{"path", r.read_range->path},
                       {"first", r.read_range->first},
                       {"last", r.read_range->last}};
  }
  if (r.effect) {
    const auto& e = *r.effect;
    v["effect"] = {
        {"path", e.path}, {"existed", e.existed}, {"after_hash", e.after_hash}};
    if (e.before) v["effect"]["before"] = *e.before;
    if (e.before_mode) v["effect"]["before_mode"] = *e.before_mode;
  }
  for (const auto& e : r.ended) {
    v["ended"].push_back({{"activity_id", e.activity_id},
                          {"status", e.status},
                          {"output", e.output},
                          {"duration_ms", e.duration_ms},
                          {"stopped", e.stopped}});
  }
  return v;
}
ToolResult DecodeResult(const json& v) {
  if (!v.is_object() || !v.contains("status")) {
    return ToolFailure(
        ToolErrorCode::kUnavailable,
        JsonValue(v, "error", "Remote execution is unavailable."));
  }
  ToolResult r;
  const int status = JsonValue(v, "status", 1),
            error = JsonValue(v, "error", 6);
  r.status = status >= 0 && status <= 3 ? static_cast<CompletionStatus>(status)
                                        : CompletionStatus::kFailed;
  r.error = error >= 0 && error <= static_cast<int>(ToolErrorCode::kInternal)
                ? static_cast<ToolErrorCode>(error)
                : ToolErrorCode::kRemoteError;
  r.output = JsonValue(v, "output", "");
  r.hint = JsonValue(v, "hint", "");
  r.body_kind = JsonValue(v, "body_kind", "text");
  r.result_chars = JsonValue(v, "result_chars", int64_t{-1});
  r.diff = JsonValue(v, "diff", "");
  r.diff_path = JsonValue(v, "diff_path", "");
  r.parts = v.value("parts", json());
  r.todo = v.value("todo", json());
  r.opened_skill = JsonValue(v, "opened_skill", "");
  r.recovery_key = JsonValue(v, "recovery_key", "");
  r.activity_terminal = JsonValue(v, "activity_terminal", false);
  r.background = JsonValue(v, "background", int64_t{0});
  const json f = v.value("facts", json::object());
  r.facts.unit = JsonValue(f, "unit", "");
  r.facts.outcome = JsonValue(f, "outcome", "");
  r.facts.reason = JsonValue(f, "reason", "");
  r.facts.no_change = JsonValue(f, "no_change", false);
  if (const json* e = JsonObject(f, "exit")) {
    r.facts.exit =
        ResultFacts::Exit{JsonValue(*e, "code", 0), JsonValue(*e, "signal", 0)};
  }
  if (f.contains("added")) r.facts.added = JsonValue(f, "added", int64_t{0});
  if (f.contains("removed")) {
    r.facts.removed = JsonValue(f, "removed", int64_t{0});
  }
  if (f.contains("items")) r.facts.items = JsonValue(f, "items", int64_t{0});
  if (const json* a = JsonObject(v, "artifact")) {
    r.artifact = ToolArtifact{JsonValue(*a, "path", ""),
                              JsonValue(*a, "bytes", uint64_t{0})};
  }
  if (const json* a = JsonObject(v, "read_range")) {
    r.read_range =
        ReadRange{JsonValue(*a, "path", ""), JsonValue(*a, "first", int64_t{0}),
                  JsonValue(*a, "last", int64_t{0})};
  }
  if (const json* e = JsonObject(v, "effect")) {
    FileEffect effect{JsonValue(*e, "path", ""),
                      JsonValue(*e, "existed", false), std::nullopt,
                      JsonValue(*e, "after_hash", "")};
    if (const std::string* b = JsonStringRef(*e, "before")) effect.before = *b;
    if (e->contains("before_mode")) {
      effect.before_mode = JsonValue(*e, "before_mode", uint32_t{0});
    }
    r.effect = std::move(effect);
  }
  if (const json* ended = JsonArray(v, "ended")) {
    for (const auto& e : *ended) {
      r.ended.push_back({JsonValue(e, "activity_id", int64_t{0}),
                         JsonValue(e, "status", 0), JsonValue(e, "output", ""),
                         JsonValue(e, "duration_ms", 0.0),
                         JsonValue(e, "stopped", false)});
    }
  }
  return r;
}
}  // namespace uagent::execution
