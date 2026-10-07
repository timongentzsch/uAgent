// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/bootstrap.h"
#include "include/app/chat.h"
#include "include/app/coordinator.h"
#include "include/app/launch.h"
#include "include/app/session.h"
#include "include/app/session_command.h"
#include "include/app/thread_link.h"
#include "include/cli.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent::session {
namespace {
// Host-claimed command["attachments"] entries ({path,id,name,mime,image})
// become worker Attachment records plus transcript display images (path
// omitted: display facts reach the client). Submit and steer share it.
bool ResolveCommandAttachments(const json& command,
                               std::vector<Attachment>& attachments,
                               json& images, std::string& error) {
  const json* paths = JsonArray(command, "attachments");
  if (!paths) return true;
  if (paths->size() > kUploadCount) {
    error = "too many attachments";
    return false;
  }
  for (const json& path : *paths) {
    Attachment attachment;
    std::string file = path.is_string() ? path.get<std::string>()
                                        : JsonValue(path, "path", "");
    if (!InspectAttachment(file, attachment, error)) return false;
    attachment.asset_id =
        std::filesystem::path(attachment.path).stem().string();
    if (path.is_object()) {
      attachment.asset_id = JsonValue(path, "id", "");
      attachment.name = JsonValue(path, "name", attachment.name);
      attachment.mime = JsonValue(path, "mime", attachment.mime);
      attachment.image = JsonValue(path, "image", false);
    }
    images.push_back(AttachmentDisplayJson(attachment));
    attachments.push_back(std::move(attachment));
  }
  return true;
}

// An ask's answers as its tool reads them: each answer's attachment_id
// becomes the path of the file the host claimed under that id. A path a
// client names itself is dropped, so an answer can only point at an upload.
std::string AskAnswers(const json& command, const std::string& text,
                       std::string& error) {
  json answers = json::parse(text, nullptr, false);
  std::vector<Attachment> attachments;
  json images = json::array();
  if (!answers.is_array() ||
      !ResolveCommandAttachments(command, attachments, images, error)) {
    return text;
  }
  for (json& answer : answers) {
    if (!answer.is_object()) continue;
    const std::string id = JsonValue(answer, "attachment_id", "");
    answer.erase("attachment_id");
    answer.erase("image");
    for (const Attachment& attachment : attachments) {
      if (!id.empty() && attachment.asset_id == id) {
        answer["image"] = attachment.path;
      }
    }
  }
  return JsonDump(answers);
}

json AttachmentsToJson(const std::vector<Attachment>& attachments) {
  json out = json::array();
  for (const Attachment& attachment : attachments) {
    out.push_back(AttachmentDisplayJson(attachment));
    out.back()["path"] = attachment.path;
  }
  return out;
}

class WorkerChannel final : public ApplicationChannel {
 public:
  WorkerChannel(std::string path, std::string id, std::string generation,
                std::string title, bool coordinator, json thread)
      : path_(std::move(path)),
        id_(std::move(id)),
        generation_(std::move(generation)),
        title_(std::move(title)),
        mail_(MailboxIdFor(path_)),
        coordinator_(coordinator) {
    if (!thread.empty()) link_.emplace(std::move(thread), path_, id_);
  }
  ~WorkerChannel() override { Close(); }

  bool Start() {
    if (!wake_.Open() ||
        !server_.Start(path_, generation_,
                       [this](const json& frame) { return Command(frame); })) {
      return false;
    }
    transient_thread_ = std::thread([this] { FlushTransientLoop(); });
    return true;
  }

  void Event(const AppEvent& event) {
    std::lock_guard transient_lock(transient_mutex_);
    if (event.type == "turn.started") {
      FlushTransientEvents();
      sent_delta_keys_.clear();
      sent_usage_ = false;
    }
    if (event.type == "response.answer.delta" ||
        event.type == "response.reasoning.delta") {
      QueueDelta(event);
      return;
    }
    if (event.type == "usage.updated") {
      QueueUsage(event);
      return;
    }
    FlushTransientEvents();
    DeliverEvent(event);
    if (event.type == "turn.completed" || event.type == "turn.stopped") {
      sent_delta_keys_.clear();
      sent_usage_ = false;
    }
  }

  void DeliverEvent(const AppEvent& event) {
    {
      std::lock_guard lock(mutex_);
      ApplySessionEvent(state_, event.type, event.data);
      if (event.type == "notice" && !ready_ &&
          notices_.size() < kBufferedNotices) {
        notices_.push_back(event.data);
      }
    }
    json data = event.data;
    if (event.type == "approval.requested") {
      // A thread's approval that Auto could not settle goes to its
      // coordinator first; one reserved for a person never does.
      if (link_ && !JsonValue(data, "mandatory_human", false)) {
        data["route"] = kRouteCoordinator;
        link_->Ask(JsonValue(data, "id", ""), "approval request",
                   "asks to run " + JsonValue(data, "tool", ""),
                   JsonValue(data, "preview", ""), title_);
      }
      std::lock_guard lock(mutex_);
      approval_ = data;
    }
    if (event.type == "turn.started") {
      std::lock_guard lock(mutex_);
      turn_active_ = true;
      BeginTurn();
      SendState();
    }
    if (event.type == "activity.status") {
      std::lock_guard lock(mutex_);
      state_.update(data);
      data["kind"] = "activity";
      data["busy"] = turn_active_;
      Send(std::move(data));
      return;
    }
    Send({{"kind", "event"},
          {"type", event.type},
          {"data", std::move(data)},
          {"time", event.time}});
  }

  void Send(json frame) {
    receipts_.Record(frame);
    StampFrame(frame, id_, generation_);
    server_.Publish(std::move(frame));
  }

  std::optional<ApplicationInput> NextInput() override {
    // A session with nothing to do stops: whatever reaches it next (a
    // message, mail, a client) starts it again.
    auto idle_since = std::chrono::steady_clock::now();
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        if (closed_ || ShutdownRequested()) {
          return std::nullopt;
        }
        if (input_) {
          auto result = std::move(input_);
          input_.reset();
          active_command_ = std::exchange(input_command_, "");
          NextControlLocked();
          return result;
        }
      }
      // Mail wakes it like a client does; the application delivers it.
      pollfd waits[] = {{wake_.read.Get(), POLLIN, 0},
                        {AbortWakeFd(), POLLIN, 0},
                        {mail_.Get(), POLLIN, 0}};
      const int ready = poll(waits, 3, PollTimeout());
      if (ready > 0 && (waits[2].revents & POLLIN)) mail_.Drain();
      if (ready < 0 && errno != EINTR) {
        return std::nullopt;
      }
      if (ready == 0) {
        if (link_) {
          std::lock_guard lock(mutex_);
          link_->Resend();
        }
        // Mail held at the spend limit is looked at again; the application
        // delivers it once the limit allows.
        if (Paused() ||
            (mail_.Get() < 0 && !PendingMail(MailboxIdFor(path_)).empty())) {
          return ApplicationInput{.wake = true};
        }
        // Idle is measured from the last input or wake.
        if (std::chrono::steady_clock::now() - idle_since < IdlePeriod() ||
            Occupied()) {
          continue;
        }
        // The web host only watches and lets go when told; a terminal stays
        // attached, and holds the session.
        Send({{"kind", "retiring"}});
        for (auto wait = kLetGo; wait.count() > 0 && server_.Clients() > 0;
             wait -= kLetGoPoll) {
          std::this_thread::sleep_for(kLetGoPoll);
        }
        std::lock_guard lock(mutex_);
        if (!input_ && server_.Clients() == 0) {
          closed_ = true;
          return std::nullopt;
        }
        idle_since = std::chrono::steady_clock::now();
        continue;
      }
      wake_.Drain();
      {
        std::lock_guard lock(mutex_);
        if (closed_ || ShutdownRequested()) {
          return std::nullopt;
        }
        if (input_) {
          continue;
        }
      }
      return ApplicationInput{.wake = true};
    }
  }

  std::string ReadInteraction(const InteractionRequest& request,
                              bool* eof) override {
    std::unique_lock lock(mutex_);
    pending_ = request.id;
    decision_ = {{"id", request.id},
                 {"kind", request.kind},
                 {"prompt", request.prompt},
                 {"options", request.options},
                 {"initial", request.initial}};
    if (JsonValue(approval_, "id", "") == request.id) {
      decision_["approval"] = approval_;
      if (approval_.contains("route")) decision_["route"] = approval_["route"];
    }
    if (request.kind == "ask") {
      decision_["questions"] = request.questions;
      // A thread's questions go to its coordinator first, as its approvals
      // do; a person is notified only if it yields or does not answer.
      if (link_) {
        decision_["route"] = kRouteCoordinator;
        link_->Ask(request.id, "question", "asks the user",
                   JsonDump(request.questions), title_);
      }
      Send({{"kind", "event"},
            {"type", "ask.requested"},
            {"data",
             {{"id", request.id},
              {"route", JsonValue(decision_, "route", "human")}}}});
    }
    state_["phase"] = "decision";
    SendState();
    const auto routed_until =
        std::chrono::steady_clock::now() + kCoordinatorDecision;
    while (!closed_ && !reply_ && !AbortRequested()) {
      const bool routed =
          JsonValue(decision_, "route", "") == kRouteCoordinator;
      lock.unlock();
      pollfd waits[] = {{wake_.read.Get(), POLLIN, 0},
                        {AbortWakeFd(), POLLIN, 0}};
      poll(waits, 2, routed ? PollTimeoutMs(routed_until) : -1);
      wake_.Drain();
      lock.lock();
      if (routed && std::chrono::steady_clock::now() >= routed_until) {
        EscalateLocked("The coordinator did not decide in time.");
      }
    }
    if (!decided_.empty()) {
      const std::string note = std::exchange(decided_, "");
      lock.unlock();
      Emit(NoticeEvent(PresentationStatus::kNeutral, note));
      lock.lock();
    }
    std::string answer = reply_.value_or("");
    *eof = closed_ || !reply_ || reply_cancelled_;
    reply_.reset();
    reply_cancelled_ = false;
    pending_.clear();
    decision_ = nullptr;
    state_["phase"] = turn_active_ ? "working" : "idle";
    SendState();
    return answer;
  }

  int WakeFd() const override { return wake_.write.Get(); }
  std::string SessionPath() const override { return path_; }
  std::string InitialTitle() const override { return title_; }
  void PublishState(const json& state, bool checkpoint) override {
    std::lock_guard lock(mutex_);
    std::string activity = JsonValue(state_, "activity", "Ready");
    std::string phase = JsonValue(state_, "phase", "idle");
    json detail = JsonValue(state_, "activity_detail", json(nullptr));
    int64_t started = JsonValue(state_, "turn_started_ms", int64_t{0});
    state_ = state;
    state_["turn_started_ms"] = started;
    state_["notices"] = notices_;
    state_["activity"] = activity;
    state_["phase"] = phase;
    state_["activity_detail"] = std::move(detail);
    if (!paused_.empty()) state_["paused"] = paused_;
    if (!checkpoint) {
      // Live accounting only: the turn keeps running, so its phase, busy
      // state and queued guidance stay untouched.
      SendState(false);
      return;
    }
    ready_ = true;
    busy_ = input_.has_value();
    if (!busy_ && turn_active_ && link_) {
      std::string answer;
      const json blocks = JsonValue(JsonValue(state, "view", json::object()),
                                    "blocks", json::array());
      for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        if (JsonValue(*block, "kind", "") != "assistant") continue;
        answer = JsonValue(*block, "text", "");
        break;
      }
      link_->Report(JsonValue(JsonValue(state, "stop", json::object()),
                              "reason", "completed"),
                    JsonValue(state_, "title", title_), answer);
    }
    if (!busy_) {
      // Completion events precede saved display/HTTP metadata. Only the final
      // application checkpoint makes the turn idle and accepts another input.
      turn_active_ = false;
      state_["activity"] = "Ready";
      state_.erase("activity_detail");
      state_["phase"] = "idle";
      ClearAbort();
      NormalizeAbortWake();
      if (auto next = SteeringState().TakeNextAutoStart()) {
        input_ = ApplicationInput{.text = std::move(next->text),
                                  .request_id = std::move(next->request_id)};
        for (const json& item : next->attachments) {
          input_->attachments.push_back(AttachmentFromJson(item));
        }
        busy_ = turn_active_ = true;
        BeginTurn();
        wake_.Wake();
      }
    }
    SendState(true);
  }
  void CompleteControl(const std::string& request,
                       const json& result) override {
    SendOutcome(request, JsonValue(result, "error", ""), &result);
  }
  void SetActivityControl(
      const std::function<json(const json&)>& control) override {
    std::lock_guard lock(control_mutex_);
    // A side question runs inside the application; it ends before the
    // application's control does.
    StopSideQuestion();
    activity_control_ = control;
    for (const auto& [request, raw] : std::exchange(early_controls_, {})) {
      CompleteControl(
          request, control ? control(raw) : json{{"error", "session closed"}});
    }
  }

  // A session wakes to notice it is idle, and a coordinator a minute later
  // while the spend limit holds its mail. Without a mailbox watch, mail is
  // looked for once a second.
  int PollTimeout() {
    if (mail_.Get() < 0) return 1000;
    if (Paused()) return static_cast<int>(kSpendRecheck.count());
    return static_cast<int>(
        std::min<int64_t>(kIdlePoll.count(),
                          std::chrono::milliseconds(IdlePeriod()).count() / 4));
  }

  // Work that a stopped runtime would lose: an input or guidance not yet
  // taken, or a command still running that is not detached.
  bool Occupied() {
    std::lock_guard lock(mutex_);
    if (input_ || SteeringState().QueuedCount() > 0) return true;
    if (link_ && link_->Owes()) return true;
    const json* activities = JsonArray(state_, "activities");
    return activities && std::ranges::any_of(*activities, ActivityRuns);
  }

  bool Paused() {
    std::lock_guard lock(mutex_);
    return !paused_.empty();
  }

  // At today's spend limit a coordinator keeps its mail pending and says so
  // in its state. Checked outside the lock: it reads the threads' files.
  bool HoldMail() override {
    const std::string pause =
        coordinator_ ? CoordinatorPause(CanonicalCwd()) : "";
    std::lock_guard lock(mutex_);
    if (pause != paused_) {
      paused_ = pause;
      if (pause.empty()) {
        state_.erase("paused");
      } else {
        state_["paused"] = pause;
      }
      SendState();
    }
    return !pause.empty();
  }

  // The routed decision becomes the user's: shown as theirs, and announced so
  // the host notifies their devices.
  void EscalateLocked(const std::string& note) {
    decision_["route"] = "human";
    decision_["note"] = note;
    if (decision_.contains("approval")) {
      decision_["approval"]["route"] = "human";
    }
    SendState();
    Send({{"kind", "event"},
          {"type", "approval.escalated"},
          {"data", {{"id", pending_}, {"note", note}}}});
  }

  void Close() {
    {
      std::lock_guard transient_lock(transient_mutex_);
      transient_stop_ = true;
      transient_changed_.notify_all();
    }
    if (transient_thread_.joinable()) transient_thread_.join();
    {
      std::lock_guard control(control_mutex_);
      StopSideQuestion();
    }
    {
      std::lock_guard transient_lock(transient_mutex_);
      FlushTransientEvents();
    }
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      RequestAbort();
    }
    wake_.Wake();
  }

 private:
  static std::string DeltaKey(const AppEvent& event) {
    return event.type + "\n" + JsonValue(event.data, "response_id", "");
  }

  void QueueDelta(const AppEvent& event) {
    const auto now = std::chrono::steady_clock::now();
    const std::string key = DeltaKey(event);
    if (JsonValue(event.data, "reset", false) ||
        sent_delta_keys_.insert(key).second) {
      FlushTransientEvents();
      DeliverEvent(event);
      return;
    }
    if (pending_delta_ && DeltaKey(*pending_delta_) != key) FlushDelta();
    if (!pending_delta_) {
      pending_delta_ = event;
      delta_started_ = now;
    } else {
      pending_delta_->data["text"] =
          JsonValue(pending_delta_->data, "text", "") +
          JsonValue(event.data, "text", "");
      if (JsonValue(event.data, "preview_truncated", false)) {
        pending_delta_->data["preview_truncated"] = true;
      }
      pending_delta_->sequence = event.sequence;
      pending_delta_->time = event.time;
    }
    if (JsonValue(pending_delta_->data, "text", "").size() >=
            kStreamBatchBytes ||
        now - delta_started_ >= kStreamBatchInterval) {
      FlushDelta();
    } else {
      transient_changed_.notify_one();
    }
  }

  void QueueUsage(const AppEvent& event) {
    const auto now = std::chrono::steady_clock::now();
    if (!sent_usage_) {
      sent_usage_ = true;
      usage_sent_ = now;
      DeliverEvent(event);
      return;
    }
    pending_usage_ = event;
    if (now - usage_sent_ >= kUsagePublishInterval) {
      FlushUsage();
    } else {
      transient_changed_.notify_one();
    }
  }

  void FlushDelta() {
    if (!pending_delta_) return;
    AppEvent event = std::move(*pending_delta_);
    pending_delta_.reset();
    DeliverEvent(event);
  }

  void FlushUsage() {
    if (!pending_usage_) return;
    AppEvent event = std::move(*pending_usage_);
    pending_usage_.reset();
    usage_sent_ = std::chrono::steady_clock::now();
    DeliverEvent(event);
  }

  void FlushTransientEvents() {
    FlushDelta();
    FlushUsage();
  }

  void FlushTransientLoop() {
    std::unique_lock lock(transient_mutex_);
    for (;;) {
      transient_changed_.wait(lock, [this] {
        return transient_stop_ || pending_delta_ || pending_usage_;
      });
      if (transient_stop_) return;
      auto deadline = std::chrono::steady_clock::time_point::max();
      if (pending_delta_) {
        deadline = std::min(deadline, delta_started_ + kStreamBatchInterval);
      }
      if (pending_usage_) {
        deadline = std::min(deadline, usage_sent_ + kUsagePublishInterval);
      }
      transient_changed_.wait_until(lock, deadline);
      if (transient_stop_) return;
      const auto now = std::chrono::steady_clock::now();
      if (pending_delta_ && now - delta_started_ >= kStreamBatchInterval) {
        FlushDelta();
      }
      if (pending_usage_ && now - usage_sent_ >= kUsagePublishInterval) {
        FlushUsage();
      }
    }
  }

  // A new user or background turn supersedes the preceding turn's stop/error.
  // Publish this transition immediately, before waiting for a model response.
  void BeginTurn() {
    state_["error"] = "";
    state_.erase("stop");
    state_["activity"] = "Working";
    state_["phase"] = "working";
    state_["turn_started_ms"] = NowMillis();
  }
  void SendState(bool checkpoint = false) {
    Send({{"kind", "state"},
          {"state", checkpoint ? state_ : LightState(state_)},
          {"busy", turn_active_},
          {"command_busy", busy_},
          {"pending", decision_},
          {"phase", JsonValue(state_, "phase", "idle")},
          {"guidance", SteeringState().QueuedCount()},
          {"completed_request_id",
           checkpoint ? std::exchange(active_command_, "") : ""},
          {"checkpoint", checkpoint}});
  }
  // A control waits its turn behind the input already there (a settings
  // screen sends several at once, and the first may arrive while the
  // runtime starts); only a running turn refuses it. True when the control
  // was taken; otherwise reports into error. The caller holds mutex_.
  bool QueueIdleControl(const std::string& request, const json& control,
                        std::string& error) {
    if (turn_active_) {
      error = "this control requires an idle session";
      return false;
    }
    if (controls_.size() >= kControlQueueLimit) {
      error = "too many settings changes are waiting";
      return false;
    }
    controls_.emplace_back(request, control);
    if (!input_) NextControlLocked();
    busy_ = true;
    wake_.Wake();
    SendState();
    return true;  // Completion carries the catalog or validated selection.
  }
  // Moves the next waiting control into the input slot. The caller holds
  // mutex_ and has seen the slot empty.
  void NextControlLocked() {
    if (controls_.empty()) return;
    input_ = ApplicationInput{.request_id = controls_.front().first,
                              .control = std::move(controls_.front().second)};
    input_command_ = controls_.front().first;
    controls_.pop_front();
  }

  std::optional<AppEvent> pending_delta_;
  std::optional<AppEvent> pending_usage_;
  std::mutex transient_mutex_;
  std::condition_variable transient_changed_;
  std::thread transient_thread_;
  std::thread side_thread_;
  std::atomic<bool> side_busy_{false};
  std::atomic<bool> side_cancel_{false};
  // Cancels a running side question and waits for its thread; the request
  // notices within one poll slice.
  void StopSideQuestion() {
    side_cancel_ = true;
    if (side_thread_.joinable()) side_thread_.join();
  }
  std::unordered_set<std::string> sent_delta_keys_;
  std::chrono::steady_clock::time_point delta_started_{};
  std::chrono::steady_clock::time_point usage_sent_{};
  bool sent_usage_ = false;
  bool transient_stop_ = false;
  // Answers a command. A result, when there is one, carries its own error.
  void SendOutcome(const std::string& request, const std::string& error,
                   const json* result = nullptr) {
    json frame = {
        {"kind", "outcome"},
        {"request_id", request},
        {"accepted", error.empty() && !(result && result->contains("error"))},
        {"error", error}};
    if (result) frame["result"] = *result;
    Send(std::move(frame));
  }

  bool Command(const json& command) {
    SessionCommand parsed;
    std::string error;
    if (!ParseSessionCommand(command, id_, generation_, parsed)) return false;
    const std::string& request = parsed.request_id;
    json previous;
    switch (receipts_.Check(command, request, previous)) {
      case ReceiptVerdict::kReplay:
        Send(std::move(previous));
        return true;
      case ReceiptVerdict::kReject:
        return false;
      case ReceiptVerdict::kNew:
        break;
    }
    SessionCommandKind kind = parsed.kind;
    // Read before the lock: it looks at the folder's session files.
    const json* files = JsonArray(parsed.raw, "attachments");
    const bool members_only = coordinator_ &&
                              kind == SessionCommandKind::kSubmit &&
                              (!files || files->empty()) &&
                              ForMembersOnly(CanonicalCwd(), parsed.text);
    std::unique_lock lock(mutex_);
    if (kind == SessionCommandKind::kSteer && !turn_active_) {
      kind = SessionCommandKind::kSubmit;
    }
    if (closed_) return false;
    if (link_ && !parsed.text.empty() &&
        (kind == SessionCommandKind::kSubmit ||
         kind == SessionCommandKind::kSteer)) {
      const std::string refused = link_->Admit(
          JsonValue(parsed.raw, "origin", "") == kRouteCoordinator);
      if (!refused.empty()) {
        SendOutcome(request, refused);
        return true;
      }
    }
    switch (kind) {
      case SessionCommandKind::kClose:
        lock.unlock();
        // A close request must acknowledge before the worker shuts down;
        // otherwise the browser waits for a receipt from a dead socket.
        CompleteControl(request, {{"operation", "close"}});
        Close();
        return true;
      case SessionCommandKind::kInterrupt:
        RequestAbort();
        wake_.Wake();
        break;
      case SessionCommandKind::kReply:
        ReplyLocked(parsed, error);
        break;
      case SessionCommandKind::kEscalate:
        if (pending_.empty() || parsed.interaction_id != pending_ || reply_ ||
            JsonValue(decision_, "route", "") != kRouteCoordinator) {
          error = "decision is stale or not with the coordinator";
        } else {
          EscalateLocked(parsed.text);
        }
        break;
      case SessionCommandKind::kSteer:
        SteerLocked(parsed, error);
        break;
      case SessionCommandKind::kRecall:
        // Pre-delivery only: the queue owns recallability, the turn owns
        // delivery. No match means the turn already took it.
        if (!SteeringState().Recall(parsed.client_request_id)) {
          error = "already delivered";
        }
        break;
      case SessionCommandKind::kRename:
        if (input_ || !ValidSessionTitle(parsed.title)) {
          error = "rename requires a valid title and an empty input queue";
        } else {
          input_ = ApplicationInput{.title = std::string(parsed.title)};
          busy_ = true;
          wake_.Wake();
          SendState();
        }
        break;
      case SessionCommandKind::kRefresh:
        SendState();
        break;
      case SessionCommandKind::kSide:
        lock.unlock();
        StartSideQuestion(request, parsed.raw);
        return true;
      case SessionCommandKind::kPermissions:
      case SessionCommandKind::kActivity:
        if (kind == SessionCommandKind::kPermissions ||
            parsed.operation != "followup") {
          lock.unlock();
          std::lock_guard control(control_mutex_);
          if (activity_control_) {
            CompleteControl(request, activity_control_(parsed.raw));
          } else {
            // The application is still starting: answered once it is there.
            early_controls_.emplace_back(request, parsed.raw);
          }
          return true;
        }
        if (QueueIdleControl(request, parsed.raw, error)) return true;
        break;
      case SessionCommandKind::kModel:
      case SessionCommandKind::kTools:
      case SessionCommandKind::kConfig:
      case SessionCommandKind::kContext:
      case SessionCommandKind::kRevert:
      case SessionCommandKind::kFork:
      case SessionCommandKind::kShare:
      case SessionCommandKind::kSelfDirective:
        if (QueueIdleControl(request, parsed.raw, error)) return true;
        break;
      case SessionCommandKind::kSubmit:
        SubmitLocked(parsed, members_only, error);
        break;
      case SessionCommandKind::kCreate:
      case SessionCommandKind::kDelete:
      case SessionCommandKind::kActivate:
      case SessionCommandKind::kUnknown:
        error = "unsupported command";
        break;
    }
    SendOutcome(request, error);
    return true;
  }

  // Answers the pending decision. The caller holds mutex_.
  void ReplyLocked(const SessionCommand& parsed, std::string& error) {
    const bool from_coordinator =
        JsonValue(parsed.raw, "origin", "") == kRouteCoordinator;
    if (pending_.empty() || parsed.interaction_id != pending_ || reply_) {
      error = "decision is stale or already answered";
      return;
    }
    if (from_coordinator &&
        JsonValue(decision_, "route", "") != kRouteCoordinator) {
      // Only a decision routed to the coordinator is its to answer.
      error = "this decision belongs to the user";
      return;
    }
    const bool ask = JsonValue(decision_, "kind", "") == "ask";
    std::string answer =
        ask ? AskAnswers(parsed.raw, parsed.text, error) : parsed.text;
    if (!error.empty()) return;
    reply_ = std::move(answer);
    reply_cancelled_ = parsed.cancelled;
    // The decision log: who decided a routed decision, and why.
    const std::string reason = JsonValue(parsed.raw, "reason", "");
    if (from_coordinator) {
      decided_ = (ask ? "coordinator answered"
                      : "coordinator decided " + parsed.text) +
                 (reason.empty() ? "" : ": " + reason);
    }
    wake_.Wake();
  }

  // Guidance only: the turn reads it at its next steering check and passive
  // waits yield on the queued message. Requesting a foreground abort here
  // would report every steer as an interruption. Files ride the same queue:
  // the turn composes them into the steered user message, so steering sees
  // what the composer showed; `queue` holds it for when the turn ends
  // instead ("Queue next"). The caller holds mutex_.
  void SteerLocked(const SessionCommand& parsed, std::string& error) {
    if (!turn_active_ || parsed.text.empty() ||
        SteeringState().QueuedCount() >= kGuidanceQueueLimit) {
      error = "guidance requires an active turn and space in its queue";
      return;
    }
    std::vector<Attachment> attachments;
    json images = json::array();
    if (ResolveCommandAttachments(parsed.raw, attachments, images, error)) {
      SteeringState().Queue(
          {.text = std::string(parsed.text),
           .request_id = parsed.client_request_id,
           .attachments = AttachmentsToJson(attachments),
           .images = std::move(images),
           .after_turn = JsonValue(parsed.raw, "queue", false)});
    }
  }

  // A model call takes seconds: it runs beside the command reader so stop,
  // interrupt and replies are never queued behind it.
  void StartSideQuestion(const std::string& request, const json& raw) {
    std::lock_guard control(control_mutex_);
    if (!activity_control_ || side_busy_) {
      CompleteControl(
          request, {{"error", activity_control_ ? "a side question is running"
                                                : "session not ready"}});
      return;
    }
    if (side_thread_.joinable()) side_thread_.join();
    side_busy_ = true;
    side_cancel_ = false;
    side_thread_ = std::thread([this, request, raw, ask = activity_control_] {
      // Its own stop flag: the main turn's Escape and the worker's shutdown
      // abort never reach it, nor does it clear theirs.
      LocalAbort local(side_cancel_);
      CompleteControl(request, ask(raw));
      side_busy_ = false;
    });
  }

  // A message during a turn is guidance; otherwise it takes the one-slot
  // input queue. The caller holds mutex_.
  // With `members_only` the message is written into a coordinator's chat for
  // the members it names: it starts no turn of the coordinator's own.
  void SubmitLocked(const SessionCommand& parsed, bool members_only,
                    std::string& error) {
    if (turn_active_ && !parsed.text.empty() && !parsed.text.starts_with("/") &&
        !parsed.has_attachments) {
      if (SteeringState().QueuedCount() >= kGuidanceQueueLimit) {
        error = "guidance queue is full";
      } else {
        SteeringState().Queue(
            std::string(parsed.text),
            JsonValue(parsed.raw, "client_request_id", parsed.request_id));
      }
      SendState();
      return;
    }
    // Reuse the one-slot queue while a non-turn control finishes. The
    // application consumes it after publishing that control's checkpoint.
    if (turn_active_ || input_) {
      error = "session is busy";
      return;
    }
    ApplicationInput input;
    input.request_id = parsed.client_request_id;
    input.text = parsed.text;
    input.quiet = members_only;
    json images = json::array();
    if (!ResolveCommandAttachments(parsed.raw, input.attachments, images,
                                   error)) {
      return;
    }
    if (input.text.empty() && input.attachments.empty()) {
      error = "empty message";
    }
    ParsedSlashCommand slash = ParseSlashCommand(input.text);
    if (slash.spec && slash.spec->Has(kClientOnly)) {
      error = "use conversation controls to navigate, branch, or close";
    }
    if (!error.empty()) return;
    ClearAbort();
    busy_ = true;
    turn_active_ = !members_only && (!input.text.starts_with("/") ||
                                     !SlashCommandPrompt(slash).empty());
    if (turn_active_) BeginTurn();
    input_ = std::move(input);
    input_command_ = parsed.request_id;
    wake_.Wake();
    SendState();
  }

  std::string path_, id_, generation_, title_;
  // Socket callbacks can run while bootstrap initializes the environment.
  Pipe wake_;
  MailboxWatch mail_;
  std::mutex mutex_, control_mutex_;
  static constexpr size_t kGuidanceQueueLimit = 8;
  static constexpr size_t kControlQueueLimit = 16;
  static constexpr auto kIdlePoll = std::chrono::milliseconds(30000);
  static constexpr auto kLetGo = std::chrono::milliseconds(2000);
  static constexpr auto kLetGoPoll = std::chrono::milliseconds(50);
  static constexpr auto kCoordinatorDecision = std::chrono::minutes(5);
  static constexpr auto kSpendRecheck = std::chrono::milliseconds(60000);
  bool closed_ = false, busy_ = true;
  bool turn_active_ = false;
  const bool coordinator_ = false;
  std::optional<ThreadLink> link_;  // set when this session is a thread
  bool reply_cancelled_ = false;
  bool ready_ = false;
  json notices_ = json::array();
  std::string paused_;  // why a coordinator holds its mail, if it does
  std::function<json(const json&)> activity_control_;
  // Permission and activity controls that arrived before the application.
  std::vector<std::pair<std::string, json>> early_controls_;
  std::optional<ApplicationInput> input_;
  // Controls waiting behind the input slot, each with its request.
  std::deque<std::pair<std::string, json>> controls_;
  std::optional<std::string> reply_;
  std::string pending_, input_command_, active_command_, decided_;
  json decision_ = nullptr, state_ = json::object(), approval_ = nullptr;
  // Destroy the transport first, while all callback state is still alive.
  ReceiptLog receipts_;
  Server server_;
};
}  // namespace

int WorkerMain(int argc, char** argv) {
  if (argc != 7 || !OpaqueId(argv[4]) || HashHex(argv[3]) != argv[4]) {
    return kWorkerBadLaunch;
  }
  (void)setsid();
  SetGracefulShutdown(true);
  std::string bytes, error;
  if (!ReadRegularFile(argv[6], kCommandBytes, bytes, error)) {
    return kWorkerBadLaunch;
  }
  unlink(argv[6]);
  json launch = json::parse(bytes, nullptr, false);
  if (!launch.is_object()) return kWorkerBadLaunch;
  // Before anyone can connect: a runtime that cannot work never answers.
  if (chdir(argv[2]) != 0) return kWorkerNoWorkspace;
  Options options = OptionsFromLaunch(launch);
  // The coordinator is known by its path; a thread's link is fixed at launch
  // and afterwards read back from its own header.
  if (argv[3] == CoordinatorPath(argv[2])) {
    options.session = {{"kind", kSessionKindCoordinator}};
  } else if (const json* role = JsonObject(launch, "session");
             role && JsonValue(*role, "kind", "") == kSessionKindThread) {
    options.session = *role;
  } else if (const json header = SessionHeader(argv[3]);
             JsonValue(header, kSessionHeaderKind, "") == kSessionKindThread) {
    options.session = {
        {"kind", kSessionKindThread},
        {"thread", JsonValue(header, kSessionHeaderThread, json::object())}};
  }
  // A thread runs sandboxed and within its budget however its runtime is
  // started again, so a restart never widens it: not even a sandbox policy
  // inherited through the environment stands in for its own. Its approval
  // mode is held to Auto where the mode is resolved (PermissionControl).
  if (JsonValue(options.session, "kind", "") == kSessionKindThread) {
    ::unsetenv("UAGENT_INTERNAL_SANDBOX");
    options.overrides["UAGENT_SANDBOX"] = "true";
    const double budget =
        ThreadBudget(JsonValue(options.session, "thread", json::object()));
    if (budget > 0) {
      options.overrides["UAGENT_SESSION_BUDGET"] = std::to_string(budget);
    }
  }
  WorkerChannel channel(argv[3], argv[4], RandomToken(16), argv[5],
                        options.Coordinator(),
                        JsonValue(options.session, "thread", json::object()));
  if (!channel.Start()) return kWorkerOwned;
  Observability observation;
  SetObservability(&observation);
  observation.EnableTerminal(false);
  observation.EnableJournal(true);
  auto subscriber = observation.Subscribe(
      [&](const AppEvent& event) { channel.Event(event); });
  BootstrapResult boot =
      Bootstrap(std::move(options), argv[0], observation, &channel);
  int status = boot.Ok() ? RunApplication(*boot.context) : boot.exit_code;
  if (!boot.Ok()) channel.Send({{"kind", "error"}, {"error", boot.error}});
  boot.context.reset();
  observation.Unsubscribe(subscriber);
  SetObservability(nullptr);
  return status;
}
}  // namespace uagent::session
