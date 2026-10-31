// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/agent/session_store.h"
#include "include/agent/transcript.h"
#include "include/app/agent_link.h"
#include "include/app/asset_store.h"
#include "include/app/bootstrap.h"
#include "include/app/launch.h"
#include "include/app/session.h"
#include "include/app/session_command.h"
#include "include/app/transient_batcher.h"
#include "include/cli/cli.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/mailbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/thread_annotations.h"
#include "include/core/time.h"
#include "include/core/turn_status.h"

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
                std::string title, const json& agent)
      : path_(std::move(path)),
        id_(std::move(id)),
        generation_(std::move(generation)),
        title_(std::move(title)),
        mail_(MailboxIdFor(path_)) {
    if (!agent.empty()) {
      link_.emplace(path_, id_,
                    AgentParentPath(path_, JsonValue(agent, "parent", "")));
    }
  }
  ~WorkerChannel() override { Close(); }

  // Zero once clients can reach it, or the WorkerExit that says why not.
  int Start() {
    if (!wake_.Open()) return kWorkerCannotListen;
    const int failed =
        server_.Start(path_, generation_,
                      [this](const json& frame) { return Command(frame); });
    if (failed) return failed;
    batcher_.Start();
    return 0;
  }

  void Event(const AppEvent& event) { batcher_.Push(event); }

  void DeliverEvent(const AppEvent& event) {
    {
      std::lock_guard lock(mutex_);
      ApplySessionEvent(state_, event.type, event.data);
      if (event.type == AppType(EventId::kNotice) && !ready_ &&
          notices_.size() < kBufferedNotices) {
        notices_.push_back(event.data);
      }
    }
    json data = event.data;
    if (event.type == AppType(EventId::kApprovalRequested)) {
      std::lock_guard lock(mutex_);
      // An agent's approval that Auto could not settle goes to its parent
      // first, unless the person started or wrote into this turn and is
      // there to answer it; one reserved for a person never does.
      if (link_ && !person_turn_ &&
          !JsonValue(data, "mandatory_human", false)) {
        data["route"] = kRouteParent;
        link_->Ask(JsonValue(data, "id", ""), "approval request",
                   "asks to run " + JsonValue(data, "tool", ""),
                   JsonValue(data, "preview", ""),
                   JsonValue(state_, "title", title_));
      }
      approval_ = data;
    }
    if (event.type == AppType(EventId::kTurnStarted)) {
      std::lock_guard lock(mutex_);
      turn_active_ = true;
      BeginTurn();
      SendState();
    }
    if (event.type == AppType(EventId::kTurnStatus)) {
      std::lock_guard lock(mutex_);
      state_.update(data);
      data.update({{"kind", "turn_status"}, {"busy", turn_active_}});
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
      // A signal is no input, so it counts as a quiet wait: an agent's exit
      // (SIGCHLD) used to start the idle period over, keeping its parent up one
      // more period per agent. What a child's end means reaches the wake
      // pipe by itself.
      if (ready <= 0) {
        if (link_) {
          std::lock_guard lock(mutex_);
          link_->Resend();
        }
        if (mail_.Get() < 0 && !PendingMail(MailboxIdFor(path_)).empty()) {
          return ApplicationInput{.wake = true};
        }
        // Idle is measured from the last input or wake.
        if (std::chrono::steady_clock::now() - idle_since < Idle() ||
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
    UniqueLock lock(mutex_);
    pending_ = request.id;
    decision_ = {{"id", request.id},
                 {"kind", request.kind},
                 {"prompt", request.prompt},
                 {"options", request.options},
                 {"description", request.description},
                 {"files", request.files},
                 {"initial", request.initial}};
    if (JsonValue(approval_, "id", "") == request.id) {
      decision_["approval"] = approval_;
      if (approval_.contains("route")) decision_["route"] = approval_["route"];
    }
    if (request.kind == "ask") {
      decision_["questions"] = request.questions;
      // An agent's questions go to its parent first, as its approvals do;
      // a person is notified only if it yields or does not answer.
      if (link_ && !person_turn_) {
        decision_["route"] = kRouteParent;
        link_->Ask(request.id, "question", "asks the person",
                   JsonDump(request.questions),
                   JsonValue(state_, "title", title_));
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
        std::chrono::steady_clock::now() +
        std::chrono::seconds(
            EnvLong("UAGENT_INTERNAL_PARENT_DECISION_S", kParentDecisionS));
    while (!closed_ && !reply_ && !AbortRequested()) {
      const bool routed = RoutedToParent(decision_);
      lock.unlock();
      pollfd waits[] = {{wake_.read.Get(), POLLIN, 0},
                        {AbortWakeFd(), POLLIN, 0}};
      poll(waits, 2, routed ? PollTimeoutMs(routed_until) : -1);
      wake_.Drain();
      lock.lock();
      if (routed && std::chrono::steady_clock::now() >= routed_until) {
        EscalateLocked("The parent agent did not decide in time.");
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
    std::string activity = JsonValue(state_, "turn_status", kIdleStatus);
    std::string phase = JsonValue(state_, "phase", "idle");
    int64_t started = JsonValue(state_, "turn_started_ms", int64_t{0});
    state_ = state;
    state_["turn_started_ms"] = started;
    state_["notices"] = notices_;
    state_["turn_status"] = activity;
    state_["phase"] = phase;
    if (!checkpoint) {
      // Live accounting only: the turn keeps running, so its phase, busy
      // state and queued guidance stay untouched.
      SendState(false);
      return;
    }
    ready_ = true;
    busy_ = input_.has_value();
    const bool finished_turn = !busy_ && turn_active_;
    if (finished_turn && link_) {
      link_->Report(JsonValue(JsonValue(state, "stop", json::object()),
                              "reason", "completed"),
                    JsonValue(state_, "title", title_),
                    JsonValue(state, "answer", ""));
    }
    if (!busy_) {
      // Completion events precede saved display/HTTP metadata. Only the final
      // application checkpoint makes the turn idle and accepts another input.
      turn_active_ = false;
      person_turn_ = false;
      state_["turn_status"] = kIdleStatus;
      state_["phase"] = "idle";
      ClearAbort();
      NormalizeAbortWake();
      if (auto next = SteeringState().TakeNextAutoStart()) {
        input_ = ApplicationInput{.text = std::move(next->text),
                                  .request_id = std::move(next->request_id),
                                  .origin = std::move(next->origin)};
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

  // How long a session waits with nothing to do before it stops. An agent
  // stops as its turn ends: what it is for, a message or mail, starts it
  // again.
  std::chrono::milliseconds Idle() const {
    return link_ ? kAgentIdle : IdlePeriod();
  }

  // A session wakes to notice it is idle. Without a mailbox watch, mail is
  // looked for once a second.
  int PollTimeout() {
    if (mail_.Get() < 0) return 1000;
    return static_cast<int>(
        std::min<int64_t>(kIdlePoll.count(), Idle().count() / 4));
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

  // The routed decision becomes the user's: shown as theirs, and announced so
  // the host notifies their devices.
  void EscalateLocked(const std::string& note) UAGENT_REQUIRES(mutex_) {
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
    batcher_.Stop();
    {
      std::lock_guard control(control_mutex_);
      StopSideQuestion();
    }
    batcher_.Flush();
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      RequestAbort();
    }
    wake_.Wake();
  }

 private:
  // A message sent by a client with no origin: the person's own, not the
  // parent's brief or decision.
  static bool FromPerson(const SessionCommand& parsed) {
    return JsonValue(parsed.raw, "origin", "").empty();
  }
  // A new user or background turn supersedes the preceding turn's stop/error.
  // Publish this transition immediately, before waiting for a model response.
  void BeginTurn() UAGENT_REQUIRES(mutex_) {
    state_["error"] = "";
    state_.erase("stop");
    state_["turn_status"] = "Working";
    state_["phase"] = "working";
    state_["turn_started_ms"] = NowMillis();
  }
  void SendState(bool checkpoint = false) UAGENT_REQUIRES(mutex_) {
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
                        std::string& error) UAGENT_REQUIRES(mutex_) {
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
  void NextControlLocked() UAGENT_REQUIRES(mutex_) {
    if (controls_.empty()) return;
    input_ = ApplicationInput{.request_id = controls_.front().first,
                              .control = std::move(controls_.front().second)};
    input_command_ = controls_.front().first;
    controls_.pop_front();
  }

  TransientBatcher batcher_{
      [this](const AppEvent& event) { DeliverEvent(event); }};
  std::thread side_thread_;
  std::atomic<bool> side_busy_{false};
  std::atomic<bool> side_cancel_{false};
  // Finish side-fork creation before its application and snapshot go away.
  void StopSideQuestion() {
    side_cancel_ = true;
    if (side_thread_.joinable()) side_thread_.join();
  }
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
    UniqueLock lock(mutex_);
    if (kind == SessionCommandKind::kSteer && !turn_active_) {
      kind = SessionCommandKind::kSubmit;
    }
    if (closed_) return false;
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
        if (StaleLocked(parsed.interaction_id) || !RoutedToParent(decision_)) {
          error = "decision is stale or not with the parent agent";
        } else {
          EscalateLocked(parsed.text);
        }
        break;
      case SessionCommandKind::kSteer:
        SteerLocked(parsed, error);
        break;
      case SessionCommandKind::kInsert:
        if (!SteeringState().InsertNow(parsed.client_request_id)) {
          error = "already delivered";
        } else {
          wake_.Wake();
          SendState();
        }
        break;
      case SessionCommandKind::kRecall:
        // Pre-delivery only: the queue owns recallability, the turn owns
        // delivery. No match means the turn already took it.
        if (!SteeringState().Recall(parsed.client_request_id)) {
          error = "already delivered";
        } else {
          SendState();
        }
        break;
      case SessionCommandKind::kRename:
        if (input_) {
          error = "rename requires an empty input queue";
        } else if (!ValidSessionTitle(parsed.title)) {
          error = InvalidSessionTitleMessage();
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
      case SessionCommandKind::kActivity: {
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
      case SessionCommandKind::kModel:
      case SessionCommandKind::kTools:
      case SessionCommandKind::kConfig:
      case SessionCommandKind::kContext:
      case SessionCommandKind::kRevert:
      case SessionCommandKind::kFork:
      case SessionCommandKind::kShare:
        if (QueueIdleControl(request, parsed.raw, error)) return true;
        break;
      case SessionCommandKind::kSubmit:
        SubmitLocked(parsed, error);
        break;
      case SessionCommandKind::kCreate:
      case SessionCommandKind::kDelete:
      case SessionCommandKind::kMove:
      case SessionCommandKind::kActivate:
      case SessionCommandKind::kUnknown:
        error = "unsupported command";
        break;
    }
    SendOutcome(request, error);
    return true;
  }

  // Whether `id` no longer names the decision waiting for an answer. The
  // caller holds mutex_.
  bool StaleLocked(const std::string& id) const UAGENT_REQUIRES(mutex_) {
    return pending_.empty() || id != pending_ || reply_;
  }

  // Answers the pending decision. The caller holds mutex_.
  void ReplyLocked(const SessionCommand& parsed, std::string& error)
      UAGENT_REQUIRES(mutex_) {
    if (StaleLocked(parsed.interaction_id)) {
      error = "decision is stale or already answered";
      return;
    }
    if (parsed.from_parent && !RoutedToParent(decision_)) {
      // Only a decision routed to the parent is its to answer.
      error = "this decision belongs to the person";
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
    if (parsed.from_parent) {
      decided_ = (ask ? "parent agent answered"
                      : "parent agent decided " + parsed.text) +
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
  void SteerLocked(const SessionCommand& parsed, std::string& error)
      UAGENT_REQUIRES(mutex_) {
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
           .request_id = parsed.client_request_id.empty()
                             ? parsed.request_id
                             : parsed.client_request_id,
           .attachments = AttachmentsToJson(attachments),
           .images = std::move(images),
           .after_turn = JsonValue(parsed.raw, "queue", false)});
      // The person wrote into the turn: what it still asks is theirs.
      if (FromPerson(parsed)) person_turn_ = true;
    }
    SendState();
  }

  // Copying a fork and its assets can take seconds: keep the command reader
  // free for stop, interrupt and replies.
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

  // A message during a turn is guidance, as a steer is; otherwise it takes
  // the one-slot input queue. The caller holds mutex_.
  void SubmitLocked(const SessionCommand& parsed, std::string& error)
      UAGENT_REQUIRES(mutex_) {
    if (turn_active_ && !parsed.text.empty() && !parsed.text.starts_with("/")) {
      SteerLocked(parsed, error);
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
    turn_active_ =
        !input.text.starts_with("/") || !SlashCommandPrompt(slash).empty();
    person_turn_ = turn_active_ && FromPerson(parsed);
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
  static constexpr auto kAgentIdle = std::chrono::milliseconds(1000);
  // How long a parent has to decide before its agent asks the person.
  static constexpr int64_t kParentDecisionS = 300;
  bool closed_ UAGENT_GUARDED_BY(mutex_) = false;
  bool busy_ UAGENT_GUARDED_BY(mutex_) = true;
  bool turn_active_ UAGENT_GUARDED_BY(mutex_) = false;
  // The running turn is the person's: their message started it or arrived
  // while it ran. Not the parent's brief, mail or queued guidance.
  bool person_turn_ UAGENT_GUARDED_BY(mutex_) = false;
  std::optional<AgentLink> link_;  // set when this session is an agent
  bool reply_cancelled_ UAGENT_GUARDED_BY(mutex_) = false;
  bool ready_ UAGENT_GUARDED_BY(mutex_) = false;
  json notices_ UAGENT_GUARDED_BY(mutex_) = json::array();
  std::function<json(const json&)> activity_control_
      UAGENT_GUARDED_BY(control_mutex_);
  // Permission and activity controls that arrived before the application.
  std::vector<std::pair<std::string, json>> early_controls_
      UAGENT_GUARDED_BY(control_mutex_);
  std::optional<ApplicationInput> input_ UAGENT_GUARDED_BY(mutex_);
  // Controls waiting behind the input slot, each with its request.
  std::deque<std::pair<std::string, json>> controls_ UAGENT_GUARDED_BY(mutex_);
  std::optional<std::string> reply_ UAGENT_GUARDED_BY(mutex_);
  std::string pending_ UAGENT_GUARDED_BY(mutex_);
  std::string input_command_ UAGENT_GUARDED_BY(mutex_);
  std::string active_command_ UAGENT_GUARDED_BY(mutex_);
  std::string decided_ UAGENT_GUARDED_BY(mutex_);
  json decision_ UAGENT_GUARDED_BY(mutex_) = nullptr;
  json state_ UAGENT_GUARDED_BY(mutex_) = json::object();
  json approval_ UAGENT_GUARDED_BY(mutex_) = nullptr;
  // Destroy the transport first, while all callback state is still alive.
  ReceiptLog receipts_;
  Server server_;
};
}  // namespace

int WorkerMain(int argc, char** argv) {
  if (argc != 7 || !OpaqueId(argv[4]) || SessionIdFor(argv[3]) != argv[4]) {
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
  // An agent's role is fixed at its start and afterwards read back from its
  // own header.
  json agent = AgentOf(SessionHeader(argv[3]));
  if (const json* role = JsonObject(launch, "session"); agent.empty() && role) {
    agent = AgentOf(*role);
  }
  if (!agent.empty()) options.session = {{"agent", agent}};
  // An agent runs sandboxed, at its depth and within its budget however its
  // runtime is started again, so a restart never widens it: neither a
  // sandbox policy nor a depth inherited through the environment stands in
  // for its own. One that may not edit only reads (bootstrap.cc). Its
  // approval mode defaults to Auto where the mode is resolved
  // (PermissionControl).
  ::unsetenv("UAGENT_INTERNAL_DEPTH");
  if (!agent.empty()) {
    ::unsetenv("UAGENT_INTERNAL_SANDBOX");
    ::setenv("UAGENT_INTERNAL_DEPTH",
             std::to_string(JsonValue(agent, "depth", int64_t{1})).c_str(), 1);
    options.overrides["UAGENT_SANDBOX"] =
        JsonValue(agent, "edits", "") == "none" ? "readonly" : "default";
    const double budget = JsonValue(agent, "budget", 0.0);
    if (budget > 0) {
      options.overrides["UAGENT_SESSION_BUDGET"] = std::to_string(budget);
    }
  }
  WorkerChannel channel(argv[3], argv[4], RandomToken(16), argv[5], agent);
  if (const int failed = channel.Start()) return failed;
  Observability observation;
  SetObservability(&observation);
  observation.EnableJournal(true);
  auto subscriber = observation.Subscribe(
      [&](const AppEvent& event) { channel.Event(event); });
  BootstrapResult boot =
      Bootstrap(std::move(options), argv[0], observation, &channel);
  int status = boot.Ok() ? RunApplication(*boot.context) : boot.exit_code;
  if (!boot.Ok()) {
    channel.Send({{"kind", "error"}, {"error", boot.error}});
    // No client may have connected yet to read that: leave the reason where
    // the one who started this runtime looks once it has ended.
    std::string ignored;
    if (WritePrivateFile(WorkerErrorPath(argv[6]), boot.error, ignored)) {
      status = kWorkerBootFailed;
    }
  }
  boot.context.reset();
  observation.Unsubscribe(subscriber);
  SetObservability(nullptr);
  return status;
}
}  // namespace uagent::session
