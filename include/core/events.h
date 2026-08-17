// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_EVENTS_H_
#define UAGENT_INCLUDE_CORE_EVENTS_H_
// One observational event spine. Emitters cannot inspect sink state or receive
// a result, so telemetry and presentation can never steer agent control flow.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/json.h"

namespace uagent {

enum class EventId : uint16_t {
  kSessionReady,
  kSessionResumed,
  kSessionEnded,
  kTurnStarted,
  kTurnStopped,
  kTurnCompleted,
  kToolCall,
  kToolResult,
  kActivityCompleted,
  kCapabilityChanged,
  kConfigChanged,
  kAnswer,
  kError,
  kResponseStarted,
  kReasoningDelta,
  kAnswerDelta,
  kResponseFinished,
  kPresentation,
};

enum class EventDurability : uint8_t { kTransient, kDurable };
enum class EventRedaction : uint8_t { kNone, kPublicProjection };

enum class PresentationKind : uint8_t { kNone, kToolCall, kToolResult };
enum class PresentationStatus : uint8_t {
  kNeutral,
  kSucceeded,
  kFailed,
  kCancelled,
};

struct PresentationArtifact {
  std::string kind;
  std::string path;
  uint64_t bytes = 0;
};

// Provider-independent terminal/replay data. Producers decide semantics once;
// presenters decide only layout, color, and verbosity.
struct PresentationRecord {
  PresentationKind kind = PresentationKind::kNone;
  PresentationStatus status = PresentationStatus::kNeutral;
  std::string id;
  std::string title;
  std::string summary;
  std::string detail;
  bool multiline = false;
  bool verbatim = false;
  bool change_display = false;
  std::vector<PresentationArtifact> artifacts;
};

struct Event {
  explicit Event(EventId event_id) : id(event_id) {}
  Event(EventId event_id, json event_data)
      : id(event_id), data(std::move(event_data)) {}

  EventId id;
  json data = nullptr;
  std::optional<PresentationRecord> presentation;
  std::string_view text;
  bool render = false;
  bool verbose = false;
  std::chrono::steady_clock::time_point anchor{};
};

struct EventPolicy {
  EventId id;
  const char* debug_name;
  const char* public_type;
  const char* journal_type;
  EventDurability durability;
  EventRedaction redaction;
};

const EventPolicy& PolicyFor(EventId id);

class TerminalPresenter;

// Bounded metadata-only session journal. It never enters model context and is
// flushed as a private sidecar beside the existing format-3 session snapshot.
class SessionJournal {
 public:
  void SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) Clear();
  }
  void Append(const Event& event, const EventPolicy& policy) noexcept;
  bool Load(const std::string& path, std::string& error);
  bool Flush(const std::string& path, std::string& error) const;
  void Clear();
  size_t Size() const { return lines_.size(); }

 private:
  static constexpr size_t kMaxEvents = 512;
  static constexpr size_t kMaxBytes = 256 * 1024;

  std::deque<std::string> lines_;
  size_t bytes_ = 0;
  int64_t sequence_ = 0;
  bool enabled_ = true;
};

// Fixed compile-time sinks: terminal, public JSONL, private debug JSONL, and
// the bounded session journal. There is deliberately no registration API.
class Observability {
 public:
  Observability();
  ~Observability();
  Observability(const Observability&) = delete;
  Observability& operator=(const Observability&) = delete;

  bool StartDebug(const std::string& path = "");
  bool StartJsonStream();
  void EnableJournal(bool enabled) { journal_.SetEnabled(enabled); }
  void Emit(Event event) noexcept;
  void Diagnostic(const std::string& name, json data = json::object()) noexcept;

  DebugSink& DebugOutput() { return debug_; }
  JsonEventStream& JsonOutput() { return json_; }
  SessionJournal& Journal() { return journal_; }

  void Flush();
  void Shutdown();

 private:
  DebugSink debug_;
  JsonEventStream json_;
  SessionJournal journal_;
  std::unique_ptr<TerminalPresenter> terminal_;
  std::mutex mutex_;
  bool shutdown_ = false;
};

// The active pointer is only a migration seam for low-level diagnostics. It is
// set once by main, has no registration surface, and never owns the runtime.
void SetObservability(Observability* observability) noexcept;
Observability* ActiveObservability() noexcept;
void Emit(Event event) noexcept;

class ResponseObservation {
 public:
  ResponseObservation(bool render, bool verbose, std::string label,
                      std::chrono::steady_clock::time_point anchor = {});
  ~ResponseObservation();
  ResponseObservation(const ResponseObservation&) = delete;
  ResponseObservation& operator=(const ResponseObservation&) = delete;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_EVENTS_H_
