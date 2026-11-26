// Copyright 2026 Timon Gentzsch

#include "include/core/events.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/logs.h"
#include "include/core/strings.h"
#include "include/core/turn_status.h"

namespace uagent {
namespace {

constexpr EventPolicy kPolicies[] = {
    {EventId::kSessionReady, "session.ready", "session_ready", nullptr,
     "session.ready", EventRedaction::kPublicProjection},
    {EventId::kSessionResumed, "session.resumed", "session_resumed", nullptr,
     "session.resumed", EventRedaction::kPublicProjection},
    {EventId::kSessionEnded, "session.ended", "session_end", nullptr,
     "session.ended", EventRedaction::kPublicProjection},
    {EventId::kTurnStarted, "turn.started", "turn_start", "turn.started",
     "turn.started", EventRedaction::kPublicProjection},
    {EventId::kTurnStopped, "turn.stopped", "turn_end", nullptr,
     "turn.completed", EventRedaction::kPublicProjection},
    {EventId::kTurnCompleted, "turn.completed", "turn_end", "usage",
     "turn.completed", EventRedaction::kPublicProjection},
    // Background work that finishes after its turn: the journal's turn record
    // is written once, so what came later is recorded beside it.
    {EventId::kTurnUsageAmended, "turn.usage_amended", "turn_usage_amended",
     nullptr, "turn.usage_amended", EventRedaction::kPublicProjection},
    {EventId::kToolCall, "tool.call", "tool_call", "tool.call", "tool.call",
     EventRedaction::kPublicProjection},
    {EventId::kToolStarted, "tool.started", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    {EventId::kToolResult, "tool.result", "tool_result", "tool.result",
     "tool.result", EventRedaction::kPublicProjection},
    {EventId::kActivityCompleted, "activity.completed", "activity_completed",
     nullptr, "activity.completed", EventRedaction::kPublicProjection},
    {EventId::kCapabilityChanged, "capability.changed", "feature_degraded",
     nullptr, "capability.changed", EventRedaction::kPublicProjection},
    {EventId::kConfigChanged, "config.changed", "config_changed", nullptr,
     "config.changed", EventRedaction::kPublicProjection},
    {EventId::kPromptChanged, "prompt.changed", "prompt_changed", nullptr,
     "prompt.changed", EventRedaction::kPublicProjection},
    {EventId::kAnswer, "answer", nullptr, "answer", nullptr,
     EventRedaction::kPublicProjection},
    {EventId::kError, "error", nullptr, "error", nullptr,
     EventRedaction::kPublicProjection},
    {EventId::kResponseStarted, "response.started", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    {EventId::kToolArguments, "response.tool_arguments", nullptr, nullptr,
     nullptr, EventRedaction::kNone},
    {EventId::kResponseRetry, "response.retry", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    {EventId::kTurnStatus, "turn.status", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    {EventId::kReasoningDelta, "response.reasoning.delta", nullptr, nullptr,
     nullptr, EventRedaction::kNone},
    {EventId::kAnswerDelta, "response.answer.delta", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    // A tool the provider ran: no journal type, so it never enters the session
    // record or model context, and no query text on the public stream.
    {EventId::kHostedToolActivity, "response.hosted_tool", "hosted_tool",
     "response.hosted_tool", nullptr, EventRedaction::kPublicProjection},
    {EventId::kResponseFinished, "response.finished", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    // One model request as it was sent and answered: what the context held
    // and what it cost, between a turn's start and its end.
    {EventId::kRequestCompleted, "request.completed", nullptr, nullptr,
     "request.completed", EventRedaction::kNone},
    {EventId::kApprovalRequested, "approval.requested", "approval_requested",
     nullptr, nullptr, EventRedaction::kNone},
    {EventId::kApprovalResolved, "approval.resolved", "approval_resolved",
     nullptr, nullptr, EventRedaction::kNone},
    {EventId::kInteractionRequested, "interaction.requested",
     "interaction_requested", nullptr, nullptr, EventRedaction::kNone},
    {EventId::kInteractionResolved, "interaction.resolved",
     "interaction_resolved", nullptr, nullptr, EventRedaction::kNone},
    {EventId::kCommandCompleted, "command.completed", "command_completed",
     nullptr, nullptr, EventRedaction::kNone},
    {EventId::kNotice, "notice", "notice", "notice", "notice",
     EventRedaction::kPublicProjection},
    {EventId::kMessageChanged, "message.changed", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    {EventId::kActivitiesChanged, "activities.changed", nullptr, nullptr,
     nullptr, EventRedaction::kNone},
    {EventId::kHttpExchange, "http.exchange", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
    {EventId::kUsageUpdated, "usage.updated", nullptr, "usage.updated", nullptr,
     EventRedaction::kNone},
    {EventId::kResponseSources, "response.sources", nullptr, nullptr, nullptr,
     EventRedaction::kNone},
};

constexpr bool ValidPolicies() {
  if (std::size(kPolicies) !=
      static_cast<size_t>(EventId::kResponseSources) + 1) {
    return false;
  }
  for (size_t index = 0; index < std::size(kPolicies); ++index) {
    if (static_cast<size_t>(kPolicies[index].id) != index) return false;
  }
  return true;
}

static_assert(ValidPolicies());

Observability* g_observability = nullptr;

json PublicProjection(const Event& event) {
  if (event.id != EventId::kToolCall) return event.data;

  json data = json::object();
  for (const char* field :
       {"turn", "step", "call_id", "response_id", "occurrence_id", "detail_id",
        "name", "arguments_digest", "issue_code", "issue_field"}) {
    if (event.data.contains(field)) data[field] = event.data[field];
  }

  json keys = json::array();
  json types = json::object();
  json parsed =
      json::parse(JsonValue(event.data, "arguments", ""), nullptr, false);
  if (parsed.is_object()) {
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
      keys.push_back(it.key());
      types[it.key()] = it.value().type_name();
    }
    for (const char* field : {"operation", "action"}) {
      auto found = parsed.find(field);
      if (found == parsed.end() || !found->is_string()) continue;
      std::string operation = AsciiLower(Trim(found->get<std::string>()));
      bool safe = !operation.empty() && operation.size() <= 64 &&
                  std::all_of(operation.begin(), operation.end(), [](char c) {
                    unsigned char byte = static_cast<unsigned char>(c);
                    return std::isalnum(byte) || c == '_' || c == '-';
                  });
      if (safe) data["operation"] = std::move(operation);
      break;
    }
  }
  data["argument_keys"] = std::move(keys);
  data["argument_types"] = std::move(types);
  return data;
}

const char* PresentationStatusName(PresentationStatus status) {
  switch (status) {
    case PresentationStatus::kNeutral:
      return "neutral";
    case PresentationStatus::kSucceeded:
      return "succeeded";
    case PresentationStatus::kFailed:
      return "failed";
    case PresentationStatus::kCancelled:
      return "cancelled";
    case PresentationStatus::kWarned:
      return "warned";
  }
  return "neutral";
}

json PresentationJson(const PresentationRecord& record) {
  json value = {{"title", record.title},
                {"status", PresentationStatusName(record.status)}};
  if (record.minor) value["minor"] = true;
  return value;
}

json AppProjection(const Event& event) {
  json data = event.data.is_object() ? event.data : json::object();
  if (!event.data.is_null() && !event.data.is_object()) {
    data["value"] = event.data;
  }
  if (!event.text.empty() && !data.contains("text")) {
    data["text"] = std::string(event.text);
  }
  if (event.presentation) {
    data["presentation"] = PresentationJson(*event.presentation);
  }
  return data;
}

json JournalProjection(const Event& event) {
  json data = json::object();
  auto copy = [&](std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
      if (event.data.contains(key)) data[key] = event.data[key];
    }
  };
  switch (event.id) {
    case EventId::kSessionReady:
      copy({"model", "route", "context_window", "capabilities", "provenance"});
      if (event.data.contains("base_url") &&
          event.data["base_url"].is_string()) {
        data["host"] = UrlHost(event.data["base_url"].get<std::string>());
      }
      break;
    case EventId::kSessionResumed:
      copy({"model", "messages"});
      break;
    case EventId::kSessionEnded:
      copy({"reason", "usage", "context_tokens"});
      break;
    case EventId::kTurnStarted:
      copy({"turn", "origin", "messages", "context_tokens"});
      break;
    case EventId::kTurnStopped:
    case EventId::kTurnCompleted:
      copy({"turn", "outcome", "steps", "tool_calls", "duration_ms", "ttt_ms",
            "tokens_per_second", "generation_ms", "generated_tokens", "usage",
            "messages", "context_tokens"});
      break;
    case EventId::kRequestCompleted:
      copy({"turn", "step", "request", "purpose", "response_id", "status",
            "http_status", "duration_ms", "ttft_ms", "messages",
            "request_bytes", "context_tokens", "context_parts", "usage"});
      break;
    case EventId::kTurnUsageAmended:
      copy({"turn", "usage", "statistics"});
      break;
    case EventId::kToolCall:
      copy({"turn", "step", "occurrence_id", "name", "arguments_digest",
            "issue_code", "issue_field"});
      break;
    case EventId::kToolResult:
      copy({"turn", "step", "occurrence_id", "name", "status",
            "completion_status"});
      // Which failure, not just that one happened.
      copy({"error_code", "issue_code", "issue_field", "activity_operation",
            "no_change", "activity_terminal", "duration_ms", "result_chars",
            "artifact_path", "artifact_bytes"});
      break;
    case EventId::kActivityCompleted:
      copy({"id", "kind", "command", "status", "output_chars"});
      break;
    case EventId::kCapabilityChanged:
      copy({"feature", "from", "to", "reason"});
      break;
    case EventId::kConfigChanged:
      copy({"permissions", "changed", "deferred", "source"});
      break;
    case EventId::kPromptChanged:
      copy({"scope", "revision"});
      break;
    default:
      break;
  }
  if (event.presentation) {
    data["presentation"] = PresentationJson(*event.presentation);
  }
  return data;
}

}  // namespace

const EventPolicy& PolicyFor(EventId id) {
  size_t index = static_cast<size_t>(id);
  return index < std::size(kPolicies) ? kPolicies[index] : kPolicies[0];
}

void SessionJournal::Append(const Event& event,
                            const EventPolicy& policy) noexcept {
  std::lock_guard lock(mutex_);
  if (!enabled_ || !policy.Durable() || !policy.journal_type) {
    return;
  }
  json record = {{"schema", "uagent.session.event.v1"},
                 {"seq", ++sequence_},
                 {"time", UtcStamp()},
                 {"type", policy.journal_type},
                 {"data", JournalProjection(event)}};
  // Until a session file takes them, the oldest go past the bound.
  if (pending_.size() >= kJournalPendingLines) pending_.pop_front();
  pending_.push_back(JsonDump(record));
}

bool SessionJournal::Load(const std::string& path, std::string& error) {
  std::lock_guard lock(mutex_);
  sequence_ = 0;
  std::ifstream input(path);
  if (!input) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || ec) {
      error = "cannot open session journal";
      return false;
    }
  }
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  // Lines that do not parse, and the unterminated last line of an append cut
  // short (the next one would join it), are dropped from the file.
  std::string kept;
  int64_t dropped = 0;
  size_t start = 0;
  for (size_t end; (end = text.find('\n', start)) != std::string::npos;
       start = end + 1) {
    const std::string_view line(text.data() + start, end - start);
    json record = json::parse(line, nullptr, false);
    if (!record.is_object() ||
        JsonValue(record, "schema", "") != "uagent.session.event.v1" ||
        !record.contains("type") || !record["type"].is_string() ||
        !record.contains("data") || !record["data"].is_object()) {
      ++dropped;
      continue;
    }
    sequence_ = std::max(sequence_, JsonValue(record, "seq", int64_t{0}));
    kept.append(line) += '\n';
  }
  if (kept.size() != text.size() && !WritePrivateFile(path, kept, error)) {
    return false;
  }
  if (dropped) {
    error = "dropped " + std::to_string(dropped) + " invalid session journal " +
            (dropped == 1 ? "line" : "lines");
  }
  // A runtime records its start before it knows which file it continues.
  for (std::string& line : pending_) {
    json record = json::parse(line);
    record["seq"] = ++sequence_;
    line = JsonDump(record);
  }
  return error.empty();
}

bool SessionJournal::Flush(const std::string& path, std::string& error) {
  std::string content;
  size_t count = 0;
  {
    std::lock_guard lock(mutex_);
    if (!enabled_ || path.empty() || pending_.empty()) return true;
    count = pending_.size();
    for (const std::string& line : pending_) content += line + '\n';
  }
  content.pop_back();
  if (!AppendPrivateLine(path, content, error, kSessionJournalBytes)) {
    return false;
  }
  // Events recorded while this was written wait for the next flush.
  std::lock_guard lock(mutex_);
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(count));
  return true;
}

void SessionJournal::SetEnabled(bool enabled) {
  std::lock_guard lock(mutex_);
  enabled_ = enabled;
  if (!enabled_) pending_.clear();
}

Observability::Observability() : activity_(std::make_unique<TurnStatus>()) {}

Observability::~Observability() {
  Shutdown();
  if (g_observability == this) g_observability = nullptr;
}

bool Observability::StartDebug(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  return debug_.Start(path);
}

bool Observability::StartJsonStream() {
  std::lock_guard<std::mutex> lock(mutex_);
  return json_.Start();
}

uint64_t Observability::Subscribe(EventSubscriber subscriber) {
  if (!subscriber) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return 0;
  uint64_t id = next_subscription_++;
  subscribers_.emplace_back(id, std::move(subscriber));
  return id;
}

void Observability::Unsubscribe(uint64_t subscription) {
  // Returning means no callback for this subscription is still in flight.
  std::lock_guard<std::recursive_mutex> delivery(delivery_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(subscribers_, [subscription](const auto& subscriber) {
    return subscriber.first == subscription;
  });
}

Event NoticeEvent(PresentationStatus status, std::string text, bool minor) {
  Event event{EventId::kNotice};
  event.presentation = PresentationRecord{status, text, minor};
  event.data = json{{"text", std::move(text)}};
  return event;
}

void Observability::Emit(Event event) noexcept {
  // Held before assigning sequence numbers, so concurrent producers deliver in
  // sequence order; the sink mutex is never held while callbacks run.
  std::lock_guard<std::recursive_mutex> delivery(delivery_mutex_);
  std::vector<std::pair<uint64_t, EventSubscriber>> subscribers;
  AppEvent app_event;
  uint64_t activity_revision = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return;
    const EventPolicy& policy = PolicyFor(event.id);
    if (activity_->Consume(event.id, event.data)) {
      activity_revision = activity_->Revision();
    }
    if (debug_.Enabled() && policy.debug_name) {
      json data = event.data;
      if (event.presentation) {
        data["presentation"] = PresentationJson(*event.presentation);
      }
      debug_.Write(policy.debug_name, std::move(data));
    }
    if (json_.Enabled() && policy.public_type) {
      json data = policy.redaction == EventRedaction::kPublicProjection
                      ? PublicProjection(event)
                      : event.data;
      json_.Emit(policy.public_type, std::move(data));
    }
    if (policy.Durable()) {
      journal_.Append(event, policy);
    }
    if (!subscribers_.empty()) {
      app_event = {++app_sequence_, UtcStamp(), policy.app_type,
                   AppProjection(event)};
      subscribers.reserve(subscribers_.size());
      subscribers = subscribers_;
    }
  }
  for (const auto& [id, subscriber] : subscribers) {
    // A callback may unsubscribe a later callback during this same delivery.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (std::none_of(
              subscribers_.begin(), subscribers_.end(),
              [id](const auto& current) { return current.first == id; })) {
        continue;
      }
    }
    subscriber(app_event);
  }
  // A subscriber may emit a newer lifecycle event recursively. Never publish
  // the older projection after it. Concurrent producers hold delivery_mutex_.
  if (activity_revision && activity_revision == activity_->Revision()) {
    Emit(Event{EventId::kTurnStatus, activity_->Status()});
  }
}

void Observability::SetRole(std::string role) {
  std::lock_guard<std::mutex> lock(mutex_);
  role_ = std::move(role);
}

void Observability::Diagnostic(Severity severity, const std::string& name,
                               const std::string& text, json data) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return;
  if (severity == Severity::kDebug) {
    if (debug_.Enabled()) debug_.Write(name, std::move(data));
    return;
  }
  json line = {{"level", severity == Severity::kError ? "error" : "warning"},
               {"role", role_},
               {"text", text},
               {"data", std::move(data)}};
  if (const std::string session = SessionIdFor(OwnSessionFile());
      !session.empty()) {
    line["session_id"] = session;
  }
  if (debug_.Enabled()) debug_.Write(name, line);
  if (!Logs(LogKind::kDiagnostics)) return;
  // Each line goes to its own day's file.
  const std::string day =
      UagentDir(kLogsDir) + "/" + UtcStamp("%Y-%m-%d") + ".jsonl";
  if (diagnostics_.Enabled() && diagnostics_.Path() != day) {
    diagnostics_.Stop();
  }
  if (diagnostics_.Enabled() || diagnostics_.Start(day)) {
    diagnostics_.Write(name, std::move(line));
  }
}

void Observability::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  debug_.Flush();
  diagnostics_.Flush();
}

void Observability::Shutdown() {
  std::lock_guard<std::recursive_mutex> delivery(delivery_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return;
  debug_.Flush();
  json_.Stop();
  debug_.Stop();
  diagnostics_.Stop();
  subscribers_.clear();
  shutdown_ = true;
}

void SetObservability(Observability* observability) noexcept {
  g_observability = observability;
}

Observability* ActiveObservability() noexcept { return g_observability; }

namespace {
thread_local bool g_quiet = false;
}  // namespace

QuietEvents::QuietEvents() { g_quiet = true; }
QuietEvents::~QuietEvents() { g_quiet = false; }

void Emit(Event event) noexcept {
  if (g_observability && !g_quiet) g_observability->Emit(std::move(event));
}

ResponseObservation::ResponseObservation(const std::string& label, json context)
    : context_(std::move(context)) {
  Event event{EventId::kResponseStarted, context_};
  event.text = label;
  Emit(std::move(event));
}

ResponseObservation::~ResponseObservation() {
  Emit(Event{EventId::kResponseFinished, std::move(context_)});
}

}  // namespace uagent
