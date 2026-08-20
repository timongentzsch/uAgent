// Copyright 2026 Timon Gentzsch

#include "include/core/events.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/ui/presentation.h"

namespace uagent {
namespace {

constexpr EventPolicy kPolicies[] = {
    {EventId::kSessionReady, "session_ready", nullptr, "session.ready",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kSessionResumed, "session_resumed", nullptr, "session.resumed",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kSessionEnded, "session_end", nullptr, "session.ended",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kTurnStarted, "turn_start", "turn.started", "turn.started",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kTurnStopped, "turn_end", nullptr, "turn.completed",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kTurnCompleted, "turn_end", "usage", "turn.completed",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kToolCall, "tool_call", "tool.call", "tool.call",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kToolResult, "tool_result", "tool.result", "tool.result",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kActivityCompleted, "activity_completed", nullptr,
     "activity.completed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kCapabilityChanged, "feature_degraded", nullptr,
     "capability.changed", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kConfigChanged, "config_changed", nullptr, "config.changed",
     EventDurability::kDurable, EventRedaction::kPublicProjection},
    {EventId::kAnswer, nullptr, "answer", nullptr, EventDurability::kTransient,
     EventRedaction::kPublicProjection},
    {EventId::kError, nullptr, "error", nullptr, EventDurability::kTransient,
     EventRedaction::kPublicProjection},
    {EventId::kResponseStarted, nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kReasoningDelta, nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kAnswerDelta, nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kResponseFinished, nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
    {EventId::kNotice, "notice", "notice", "notice", EventDurability::kDurable,
     EventRedaction::kPublicProjection},
    {EventId::kPresentation, nullptr, nullptr, nullptr,
     EventDurability::kTransient, EventRedaction::kNone},
};

constexpr bool ValidPolicies() {
  if (std::size(kPolicies) != static_cast<size_t>(EventId::kPresentation) + 1)
    return false;
  for (size_t index = 0; index < std::size(kPolicies); ++index) {
    if (static_cast<size_t>(kPolicies[index].id) != index) return false;
  }
  return true;
}

static_assert(ValidPolicies());

Observability* g_observability = nullptr;

json PublicProjection(const Event& event) {
  json data = event.data;
  if (event.id == EventId::kToolCall && data.contains("arguments") &&
      data["arguments"].is_string()) {
    json parsed =
        json::parse(data["arguments"].get<std::string>(), nullptr, false);
    if (!parsed.is_discarded()) data["parsed_arguments"] = std::move(parsed);
  }
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
  AddArtifacts(value, record.artifacts);
  return value;
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
      break;
    case EventId::kTurnStopped:
    case EventId::kTurnCompleted:
      copy("turn");
      copy("outcome");
      copy("steps");
      copy("usage");
      break;
    case EventId::kToolCall:
      copy("turn");
      copy("step");
      copy("id");
      copy("name");
      copy("text_protocol");
      break;
    case EventId::kToolResult:
      copy("turn");
      copy("step");
      copy("id");
      copy("name");
      copy("status");
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
      copy("changed");
      copy("deferred");
      copy("source");
      break;
    default:
      break;
  }
  if (event.presentation) {
    data["presentation"] = JournalPresentationJson(*event.presentation);
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
  return AtomicWriteFile(path, content, 0600, /*preserve_mode=*/false, error);
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
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return;
  if (terminal_) terminal_->Finish();
  debug_.Flush();
  json_.Stop();
  debug_.Stop();
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
    bool render, bool verbose, std::string label,
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
