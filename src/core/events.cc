// Copyright 2026 Timon Gentzsch

#include "include/core/events.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/ui/presentation.h"

namespace uagent {
namespace {

constexpr EventPolicy kPolicies[] = {
    {EventId::kSessionReady, "session.ready", "session_ready", nullptr,
     "session.ready", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kSessionResumed, "session.resumed", "session_resumed", nullptr,
     "session.resumed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kSessionEnded, "session.ended", "session_end", nullptr,
     "session.ended", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kTurnStarted, "turn.started", "turn_start", "turn.started",
     "turn.started", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kTurnStopped, "turn.stopped", "turn_end", nullptr,
     "turn.completed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kTurnCompleted, "turn.completed", "turn_end", "usage",
     "turn.completed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kToolCall, "tool.call", "tool_call", "tool.call", "tool.call",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kToolResult, "tool.result", "tool_result", "tool.result",
     "tool.result", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kActivityCompleted, "activity.completed", "activity_completed",
     nullptr, "activity.completed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kCapabilityChanged, "capability.changed", "feature_degraded",
     nullptr, "capability.changed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kConfigChanged, "config.changed", "config_changed", nullptr,
     "config.changed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kPromptChanged, "prompt.changed", "prompt_changed", nullptr,
     "prompt.changed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kAnswer, "answer", nullptr, "answer", nullptr,
     EventDurability::kTransient, EventRedaction::kPublicProjection},
    {EventId::kError, "error", nullptr, "error", nullptr,
     EventDurability::kTransient, EventRedaction::kPublicProjection},
    {EventId::kResponseStarted, "response.started", nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kReasoningDelta, "response.reasoning.delta", nullptr, nullptr,
     nullptr, EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kAnswerDelta, "response.answer.delta", nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    // A tool the provider ran, not one this agent did: no journal type, so it
    // never enters the session record or model context, and no query text, so
    // the public stream carries the fact of a search and not its subject.
    {EventId::kHostedToolActivity, "response.hosted_tool", "hosted_tool",
     "response.hosted_tool", nullptr, EventDurability::kTransient,
     EventRedaction::kPublicProjection},
    {EventId::kResponseFinished, "response.finished", nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kApprovalRequested, "approval.requested", "approval_requested",
     nullptr, nullptr, EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kApprovalResolved, "approval.resolved", "approval_resolved",
     nullptr, nullptr, EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kInteractionRequested, "interaction.requested",
     "interaction_requested", nullptr, nullptr, EventDurability::kTransient,
     EventRedaction::kNone},
    {EventId::kInteractionResolved, "interaction.resolved",
     "interaction_resolved", nullptr, nullptr, EventDurability::kTransient,
     EventRedaction::kNone},
    {EventId::kCommandCompleted, "command.completed", "command_completed",
     nullptr, nullptr, EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kNotice, "notice", "notice", "notice", "notice",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kMessageChanged, "message.changed", nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kActivitiesChanged, "activities.changed", nullptr, nullptr,
     nullptr, EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kHttpExchange, "http.exchange", nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kPresentation, "ui.presentation", nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
};

constexpr bool ValidPolicies() {
  if (std::size(kPolicies) != static_cast<size_t>(EventId::kPresentation) + 1) {
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
  for (const char* field : {"turn", "step", "id", "name", "arguments_digest",
                            "issue_code", "issue_field"}) {
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

void AddArtifacts(json& value,
                  const std::vector<PresentationArtifact>& artifacts) {
  if (artifacts.empty()) return;
  value["artifacts"] = json::array();
  for (const PresentationArtifact& artifact : artifacts) {
    value["artifacts"].push_back({{"kind", artifact.kind},
                                  {"path", artifact.path},
                                  {"bytes", artifact.bytes}});
  }
}

json PresentationJson(const PresentationRecord& record) {
  json value = {{"id", record.id},
                {"title", record.title},
                {"summary", record.summary},
                {"status", PresentationStatusName(record.status)}};
  if (!record.detail.empty()) {
    value["detail"] = Utf8Trunc(record.detail, size_t{4096});
  }
  if (!record.change.empty()) {
    value["change"] = Utf8Trunc(record.change, size_t{4096});
  }
  AddArtifacts(value, record.artifacts);
  return value;
}

json AppProjection(const Event& event) {
  json data = event.data.is_object() ? event.data : json::object();
  if (!event.data.is_null() && !event.data.is_object()) {
    data["value"] = event.data;
  }
  if (!event.text.empty()) data["text"] = std::string(event.text);
  if (event.presentation) {
    data["presentation"] = PresentationJson(*event.presentation);
  }
  return data;
}

json JournalPresentationJson(const PresentationRecord& record) {
  json value = {{"id", record.id},
                {"title", record.title},
                {"status", PresentationStatusName(record.status)}};
  AddArtifacts(value, record.artifacts);
  return value;
}

json JournalProjection(const Event& event) {
  json data = json::object();
  auto copy = [&](const char* key) {
    if (event.data.contains(key)) data[key] = event.data[key];
  };
  switch (event.id) {
    case EventId::kSessionReady:
      copy("model");
      copy("route");
      copy("context_window");
      copy("capabilities");
      copy("toolset");
      copy("provenance");
      if (event.data.contains("base_url") &&
          event.data["base_url"].is_string()) {
        data["host"] = UrlHost(event.data["base_url"].get<std::string>());
      }
      break;
    case EventId::kSessionResumed:
      copy("model");
      copy("messages");
      break;
    case EventId::kSessionEnded:
      copy("reason");
      copy("usage");
      copy("context_tokens");
      break;
    case EventId::kTurnStarted:
      copy("turn");
      copy("origin");
      copy("messages");
      copy("context_tokens");
      break;
    case EventId::kTurnStopped:
    case EventId::kTurnCompleted:
      copy("turn");
      copy("outcome");
      copy("steps");
      copy("tool_calls");
      copy("duration_ms");
      copy("ttt_ms");
      copy("tokens_per_second");
      copy("generation_ms");
      copy("generated_tokens");
      copy("usage");
      copy("session_usage");
      copy("messages");
      copy("context_tokens");
      break;
    case EventId::kToolCall:
      copy("turn");
      copy("step");
      copy("id");
      copy("name");
      copy("arguments_digest");
      copy("issue_code");
      copy("issue_field");
      break;
    case EventId::kToolResult:
      copy("turn");
      copy("step");
      copy("id");
      copy("name");
      copy("status");
      copy("completion_status");
      // Which failure, not just that one happened: a journal that cannot name
      // the category cannot tell the next iteration what to fix.
      copy("error_code");
      copy("issue_code");
      copy("issue_field");
      copy("activity_operation");
      copy("no_change");
      copy("activity_terminal");
      copy("duration_ms");
      copy("result_chars");
      copy("artifact_path");
      copy("artifact_bytes");
      break;
    case EventId::kActivityCompleted:
      copy("id");
      copy("kind");
      copy("status");
      copy("output_chars");
      break;
    case EventId::kCapabilityChanged:
      copy("feature");
      copy("from");
      copy("to");
      copy("reason");
      break;
    case EventId::kConfigChanged:
      copy("permissions");
      copy("changed");
      copy("deferred");
      copy("source");
      break;
    case EventId::kPromptChanged:
      copy("scope");
      copy("revision");
      break;
    default:
      break;
  }
  if (event.presentation) {
    data["presentation"] = JournalPresentationJson(*event.presentation);
  }
  return data;
}

// A headless child prints nothing until its final answer, so a parent that
// delegated a long task cannot tell work from a stall. When the parent asks
// for it, every durable event is echoed as one unbuffered stderr line: the
// child's stderr is already dup2'd into the activity log the parent polls
// (tools/shell.cc), so live traceability needs no second channel and no change
// to the stdout answer contract.
// One line per event, kept narrow enough to stay readable in a polled log.
constexpr size_t kProgressLineChars = 120;

void EchoHeadlessProgress(const Event& event, const EventPolicy& policy) {
  static const bool kEnabled = HeadlessProgressEnabled();
  if (!kEnabled || !policy.journal_type || !event.presentation) return;
  const PresentationRecord& record = *event.presentation;
  std::string line = record.title;
  if (!record.summary.empty()) {
    line += line.empty() ? record.summary : " · " + record.summary;
  }
  if (line.empty()) line = policy.journal_type;
  fprintf(stderr, "%s%s\n", kHeadlessProgressPrefix,
          Utf8Trunc(TerminalSafe(line), kProgressLineChars).c_str());
}

}  // namespace

const EventPolicy& PolicyFor(EventId id) {
  size_t index = static_cast<size_t>(id);
  return index < std::size(kPolicies) ? kPolicies[index] : kPolicies[0];
}

void SessionJournal::Append(const Event& event,
                            const EventPolicy& policy) noexcept {
  if (!enabled_ || policy.durability != EventDurability::kDurable ||
      !policy.journal_type) {
    return;
  }
  json record = {{"schema", "uagent.session.event.v1"},
                 {"seq", ++sequence_},
                 {"time", UtcStamp()},
                 {"type", policy.journal_type},
                 {"data", JournalProjection(event)}};
  std::string line = JsonDump(record);
  size_t line_bytes = line.size() + 1;
  if (line_bytes > kMaxBytes) return;
  while (!lines_.empty() &&
         (lines_.size() >= kMaxEvents || bytes_ + line_bytes > kMaxBytes)) {
    bytes_ -= lines_.front().size() + 1;
    lines_.pop_front();
  }
  bytes_ += line_bytes;
  lines_.push_back(std::move(line));
}

bool SessionJournal::Load(const std::string& path, std::string& error) {
  if (!enabled_) {
    Clear();
    return true;
  }
  Clear();
  std::ifstream input(path);
  if (!input) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
      return true;
    }
    error = "cannot open session journal";
    return false;
  }
  std::deque<std::string> loaded;
  size_t bytes = 0;
  int64_t sequence = 0;
  std::string line;
  while (std::getline(input, line)) {
    json record = json::parse(line, nullptr, false);
    if (!record.is_object() ||
        JsonValue(record, "schema", "") != "uagent.session.event.v1" ||
        !record.contains("type") || !record["type"].is_string() ||
        !record.contains("data") || !record["data"].is_object()) {
      error = "session journal contains an invalid record";
      return false;
    }
    sequence = std::max(sequence, JsonValue(record, "seq", int64_t{0}));
    size_t line_bytes = line.size() + 1;
    if (line_bytes > kMaxBytes) continue;
    while (!loaded.empty() &&
           (loaded.size() >= kMaxEvents || bytes + line_bytes > kMaxBytes)) {
      bytes -= loaded.front().size() + 1;
      loaded.pop_front();
    }
    bytes += line_bytes;
    loaded.push_back(std::move(line));
  }
  lines_ = std::move(loaded);
  bytes_ = bytes;
  sequence_ = sequence;
  return true;
}

bool SessionJournal::Flush(const std::string& path, std::string& error) const {
  if (!enabled_ || path.empty()) return true;
  std::string content;
  content.reserve(bytes_);
  for (const std::string& line : lines_) content += line + '\n';
  return AtomicWriteFile(path, content, kPrivateFileMode,
                         /*preserve_mode=*/false, error);
}

void SessionJournal::Clear() {
  lines_.clear();
  bytes_ = 0;
  sequence_ = 0;
}

Observability::Observability()
    : terminal_(std::make_unique<TerminalPresenter>()) {}

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

void Observability::EnableTerminal(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (enabled == static_cast<bool>(terminal_)) return;
  if (terminal_) terminal_->Finish();
  if (enabled) {
    terminal_ = std::make_unique<TerminalPresenter>();
  } else {
    terminal_.reset();
  }
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

Event NoticeEvent(PresentationStatus status, std::string text) {
  Event event{EventId::kNotice};
  event.presentation = PresentationRecord{};
  event.presentation->kind = PresentationKind::kNotice;
  event.presentation->status = status;
  event.presentation->title = text;
  event.data = json{{"text", std::move(text)}};
  // Notices printed unconditionally before this existed, including headless.
  event.render = true;
  return event;
}

void Observability::Emit(Event event) noexcept {
  // Acquire before assigning sequence numbers so concurrent producers deliver
  // AppEvents in sequence order. Sink state remains protected separately and
  // is never held while application callbacks run.
  std::lock_guard<std::recursive_mutex> delivery(delivery_mutex_);
  std::vector<std::pair<uint64_t, EventSubscriber>> subscribers;
  AppEvent app_event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return;
    const EventPolicy& policy = PolicyFor(event.id);
    if (terminal_) terminal_->Consume(event);
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
    if (policy.durability == EventDurability::kDurable) {
      journal_.Append(event, policy);
      EchoHeadlessProgress(event, policy);
    }
    if (!subscribers_.empty()) {
      app_event = {++app_sequence_, UtcStamp(), policy.app_type,
                   AppProjection(event),
                   policy.durability == EventDurability::kDurable};
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
}

void Observability::Diagnostic(const std::string& name, json data) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!shutdown_ && debug_.Enabled()) debug_.Write(name, std::move(data));
}

void Observability::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal_) terminal_->Finish();
  debug_.Flush();
}

void Observability::Shutdown() {
  std::lock_guard<std::recursive_mutex> delivery(delivery_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return;
  if (terminal_) terminal_->Finish();
  debug_.Flush();
  json_.Stop();
  debug_.Stop();
  subscribers_.clear();
  shutdown_ = true;
}

void SetObservability(Observability* observability) noexcept {
  g_observability = observability;
}

Observability* ActiveObservability() noexcept { return g_observability; }

void Emit(Event event) noexcept {
  if (g_observability) g_observability->Emit(std::move(event));
}

ResponseObservation::ResponseObservation(
    bool render, bool verbose, const std::string& label,
    std::chrono::steady_clock::time_point anchor) {
  Event event{EventId::kResponseStarted};
  event.render = render;
  event.verbose = verbose;
  event.text = label;
  event.anchor = anchor;
  Emit(std::move(event));
}

ResponseObservation::~ResponseObservation() {
  Emit(Event{EventId::kResponseFinished});
}

}  // namespace uagent
