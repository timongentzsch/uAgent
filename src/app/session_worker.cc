// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/bootstrap.h"
#include "include/app/session.h"
#include "include/app/session_command.h"
#include "include/cli.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/steering.h"

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
    images.push_back({{"id", attachment.asset_id},
                      {"name", attachment.name},
                      {"mime", attachment.mime},
                      {"bytes", attachment.bytes},
                      {"image", attachment.image}});
    attachments.push_back(std::move(attachment));
  }
  return true;
}

json AttachmentsToJson(const std::vector<Attachment>& attachments) {
  json out = json::array();
  for (const Attachment& attachment : attachments) {
    out.push_back({{"path", attachment.path},
                   {"name", attachment.name},
                   {"mime", attachment.mime},
                   {"bytes", attachment.bytes},
                   {"image", attachment.image},
                   {"id", attachment.asset_id}});
  }
  return out;
}

class WorkerChannel final : public ApplicationChannel {
 public:
  WorkerChannel(std::string path, std::string id, std::string generation,
                std::string title, bool delegated)
      : path_(std::move(path)),
        id_(std::move(id)),
        generation_(std::move(generation)),
        title_(std::move(title)),
        delegated_(delegated) {}
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
      std::lock_guard lock(mutex_);
      approval_ = data;
    }
    if (event.type == "turn.started") {
      std::lock_guard lock(mutex_);
      turn_active_ = true;
      BeginTurn();
      SendState();
    }
    std::string phase;
    if (event.type == "turn.started") {
      phase = "working";
    } else if (event.type == "response.started") {
      phase = "waiting";
    } else if (event.type == "response.reasoning.delta") {
      phase = "thinking";
    } else if (event.type == "response.answer.delta") {
      phase = "responding";
    } else if (event.type == "tool.call") {
      phase = "tool";
    } else if (event.type == "tool.result") {
      phase = "working";
    } else if (event.type == "response.hosted_tool") {
      phase = "searching";
    } else if (event.type == "turn.completed" || event.type == "turn.stopped") {
      phase = "finishing";
    }
    if (!phase.empty()) {
      std::string activity = phase == "waiting"      ? "Waiting for model"
                             : phase == "thinking"   ? "Thinking"
                             : phase == "responding" ? "Responding"
                             : phase == "tool"
                                 ? "Running " + JsonValue(data, "name", "tool")
                             : phase == "searching" ? "Searching"
                             : phase == "finishing" ? "Finishing"
                                                    : "Working";
      std::lock_guard lock(mutex_);
      if (JsonValue(state_, "phase", "") != phase ||
          JsonValue(state_, "activity", "") != activity) {
        state_["phase"] = phase;
        state_["activity"] = activity;
        Send({{"kind", "activity"},
              {"activity", activity},
              {"phase", phase},
              {"busy", turn_active_}});
      }
    }
    Send({{"kind", "event"},
          {"type", event.type},
          {"data", std::move(data)},
          {"time", event.time}});
  }

  void Send(json frame) {
    receipts_.Record(frame);
    frame["v"] = kProtocol;
    frame["session_id"] = id_;
    frame["generation"] = generation_;
    server_.Publish(std::move(frame));
  }

  std::optional<ApplicationInput> NextInput() override {
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
          return result;
        }
      }
      pollfd waits[] = {{wake_.read.Get(), POLLIN, 0},
                        {AbortWakeFd(), POLLIN, 0}};
      if (poll(waits, 2, -1) < 0 && errno != EINTR) {
        return std::nullopt;
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
    }
    state_["phase"] = "decision";
    SendState();
    while (!closed_ && !reply_ && !AbortRequested()) {
      lock.unlock();
      pollfd waits[] = {{wake_.read.Get(), POLLIN, 0},
                        {AbortWakeFd(), POLLIN, 0}};
      poll(waits, 2, -1);
      wake_.Drain();
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
    state_ = state;
    state_["notices"] = notices_;
    state_["activity"] = activity;
    state_["phase"] = phase;
    if (!checkpoint) {
      // Live accounting only: the turn keeps running, so its phase, busy
      // state and queued guidance stay untouched.
      SendState(false);
      return;
    }
    ready_ = true;
    busy_ = input_.has_value();
    if (!busy_) {
      // Completion events precede saved display/HTTP metadata. Only the final
      // application checkpoint makes the turn idle and accepts another input.
      turn_active_ = false;
      state_["activity"] = "Ready";
      state_["phase"] = "idle";
      ClearAbort();
      NormalizeAbortWake();
      auto queued = SteeringState().TakeAutoStartMessages();
      if (!queued.empty()) {
        input_ = ApplicationInput{
            .text = std::move(queued.front().text),
            .request_id = std::move(queued.front().request_id)};
        for (const json& item : queued.front().attachments) {
          input_->attachments.push_back(AttachmentFromJson(item));
        }
        for (size_t i = 1; i < queued.size(); ++i) {
          SteeringState().Queue(
              std::move(queued[i].text), std::move(queued[i].request_id),
              queued[i].auto_start, std::move(queued[i].attachments),
              std::move(queued[i].images));
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
    Send({{"kind", "outcome"},
          {"request_id", request},
          {"accepted", !result.contains("error")},
          {"error", JsonValue(result, "error", "")},
          {"result", result}});
  }
  void SetActivityControl(
      const std::function<json(const json&)>& control) override {
    std::lock_guard lock(control_mutex_);
    activity_control_ = control;
  }

  void Close() {
    {
      std::lock_guard transient_lock(transient_mutex_);
      transient_stop_ = true;
      transient_changed_.notify_all();
    }
    if (transient_thread_.joinable()) transient_thread_.join();
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
    if (!sent_delta_keys_.contains(key)) {
      FlushTransientEvents();
      sent_delta_keys_.insert(key);
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
  }
  void SendState(bool checkpoint = false) {
    Send({{"kind", "state"},
          {"state", state_},
          {"busy", turn_active_},
          {"command_busy", busy_},
          {"pending", decision_},
          {"pending_decision", decision_},
          {"phase", JsonValue(state_, "phase", "idle")},
          {"guidance", SteeringState().QueuedCount()},
          {"completed_request_id",
           checkpoint ? std::exchange(active_command_, "") : ""},
          {"checkpoint", checkpoint}});
  }
  // Reuse the one-slot queue while a preceding non-turn control finishes.
  // True when the control was queued; otherwise reports into error. The
  // caller holds mutex_.
  bool QueueIdleControl(const std::string& request, const json& control,
                        std::string& error) {
    if (turn_active_ || input_) {
      error = "this control requires an idle session";
      return false;
    }
    input_ = ApplicationInput{.request_id = request, .control = control};
    input_command_ = request;
    busy_ = true;
    wake_.Wake();
    SendState();
    return true;  // Completion carries the catalog or validated selection.
  }

  std::optional<AppEvent> pending_delta_;
  std::optional<AppEvent> pending_usage_;
  std::mutex transient_mutex_;
  std::condition_variable transient_changed_;
  std::thread transient_thread_;
  std::unordered_set<std::string> sent_delta_keys_;
  std::chrono::steady_clock::time_point delta_started_{};
  std::chrono::steady_clock::time_point usage_sent_{};
  bool sent_usage_ = false;
  bool transient_stop_ = false;
  bool Command(const json& command) {
    SessionCommand parsed;
    std::string error;
    if (!ParseSessionCommand(command, id_, generation_, parsed, error)) {
      return false;
    }
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
    std::unique_lock lock(mutex_);
    if (kind == SessionCommandKind::kSteer && !turn_active_) {
      kind = SessionCommandKind::kSubmit;
    }
    if (closed_) return false;
    switch (kind) {
      case SessionCommandKind::kClose: {
        lock.unlock();
        // A close request must acknowledge before the worker shuts down;
        // otherwise the browser waits for a receipt from a dead socket.
        CompleteControl(request, {{"operation", "close"}});
        Close();
        return true;
      }
      case SessionCommandKind::kInterrupt: {
        RequestAbort();
        wake_.Wake();
        break;
      }
      case SessionCommandKind::kReply: {
        if (pending_.empty() || parsed.interaction_id != pending_ || reply_) {
          error = "decision is stale or already answered";
        } else {
          reply_ = parsed.text;
          reply_cancelled_ = parsed.cancelled;
          wake_.Wake();
        }
        break;
      }
      case SessionCommandKind::kSteer:
      case SessionCommandKind::kGuide: {
        const bool guide = kind == SessionCommandKind::kGuide;
        if (guide && parsed.text.empty()) {
          // Mail-first ping: payload is on disk, just drain it into the queue.
          // Best-effort from the worker thread; PrepareStep drains again
          // anyway.
          lock.unlock();
          DrainCollaboratorMailIntoSteering();
          wake_.Wake();
          return true;
        }
        if (!turn_active_ || parsed.text.empty() ||
            SteeringState().QueuedCount() >= 8) {
          error = "guidance requires an active turn and space in its queue";
        } else {
          // Guidance only: the turn reads it at its next steering check and
          // passive waits yield on the queued message. Requesting a foreground
          // abort here would report every steer as an interruption.
          // Files ride the same queue: the turn composes them into the
          // steered user message, so steering sees what the composer showed.
          std::vector<Attachment> attachments;
          json images = json::array();
          if (ResolveCommandAttachments(parsed.raw, attachments, images,
                                        error)) {
            SteeringState().Queue(
                std::string(parsed.text), parsed.client_request_id, !guide,
                AttachmentsToJson(attachments), std::move(images));
          }
        }
        break;
      }
      case SessionCommandKind::kRecall: {
        // Pre-delivery only: the queue owns recallability, the turn owns
        // delivery. No match means the turn already took it.
        if (!SteeringState().Recall(parsed.client_request_id)) {
          error = "already delivered";
        }
        break;
      }
      case SessionCommandKind::kRename: {
        if (input_ || !ValidSessionTitle(parsed.title)) {
          error = "rename requires a valid title and an empty input queue";
        } else {
          input_ = ApplicationInput{.title = std::string(parsed.title)};
          busy_ = true;
          wake_.Wake();
          SendState();
        }
        break;
      }
      case SessionCommandKind::kRefresh: {
        SendState();
        break;
      }
      case SessionCommandKind::kPermissions:
      case SessionCommandKind::kActivity: {
        if (kind == SessionCommandKind::kPermissions ||
            parsed.operation != "followup") {
          lock.unlock();
          std::lock_guard control(control_mutex_);
          CompleteControl(request, activity_control_
                                       ? activity_control_(parsed.raw)
                                       : json{{"error", "session not ready"}});
          return true;
        }
        if (QueueIdleControl(request, parsed.raw, error)) return true;
        break;
      }
      case SessionCommandKind::kModel:
      case SessionCommandKind::kTools:
      case SessionCommandKind::kConfig:
      case SessionCommandKind::kContext:
      case SessionCommandKind::kFork:
      case SessionCommandKind::kRewind:
      case SessionCommandKind::kShare:
      case SessionCommandKind::kPrompt: {
        if (QueueIdleControl(request, parsed.raw, error)) return true;
        break;
      }
      case SessionCommandKind::kSubmit: {
        if (turn_active_ && !parsed.text.empty() &&
            !parsed.text.starts_with("/") && !parsed.has_attachments) {
          if (SteeringState().QueuedCount() >= 8) {
            error = "guidance queue is full";
          } else {
            SteeringState().Queue(
                std::string(parsed.text),
                JsonValue(parsed.raw, "client_request_id", request));
          }
          SendState();
          break;
        }
        // Reuse the one-slot queue while a non-turn control finishes. The
        // application consumes it after publishing that control's checkpoint.
        if (turn_active_ || input_) {
          error = "session is busy";
        } else {
          ApplicationInput input;
          if (delegated_) {
            input.budget = parsed.budget;
          }
          input.request_id = parsed.client_request_id;
          input.text = parsed.text;
          json images = json::array();
          if (!ResolveCommandAttachments(parsed.raw, input.attachments, images,
                                         error)) {
            Send({{"kind", "outcome"},
                  {"request_id", request},
                  {"accepted", false},
                  {"error", error}});
            return true;
          }
          if (input.text.empty() && input.attachments.empty()) {
            error = "empty message";
          }
          ParsedSlashCommand slash = ParseSlashCommand(input.text);
          if (slash.spec && (slash.spec->id == SlashCommandId::kReset ||
                             slash.spec->id == SlashCommandId::kClear ||
                             slash.spec->id == SlashCommandId::kFork ||
                             slash.spec->id == SlashCommandId::kRewind ||
                             slash.spec->id == SlashCommandId::kShare ||
                             slash.spec->id == SlashCommandId::kSessions ||
                             slash.spec->id == SlashCommandId::kQuit)) {
            error = "use conversation controls to navigate, branch, or close";
          }
          if (error.empty()) {
            ClearAbort();
            busy_ = true;
            turn_active_ = !input.text.starts_with("/") ||
                           !SlashCommandPrompt(slash).empty();
            if (turn_active_) BeginTurn();
            input_ = std::move(input);
            input_command_ = request;
            wake_.Wake();
            SendState();
          }
        }
        break;
      }
      case SessionCommandKind::kUnknown: {
        error = "unsupported command";
        break;
      }
    }
    Send({{"kind", "outcome"},
          {"request_id", request},
          {"accepted", error.empty()},
          {"error", error}});
    return true;
  }

  std::string path_, id_, generation_, title_;
  // Socket callbacks can run while bootstrap initializes the environment.
  const bool delegated_;
  Pipe wake_;
  std::mutex mutex_, control_mutex_;
  bool closed_ = false, busy_ = true;
  bool turn_active_ = false;
  bool reply_cancelled_ = false;
  bool ready_ = false;
  json notices_ = json::array();
  std::function<json(const json&)> activity_control_;
  std::optional<ApplicationInput> input_;
  std::optional<std::string> reply_;
  std::string pending_, input_command_, active_command_;
  json decision_ = nullptr, state_ = json::object(), approval_ = nullptr;
  // Destroy the transport first, while all callback state is still alive.
  ReceiptLog receipts_;
  Server server_;
};
}  // namespace

int WorkerMain(int argc, char** argv) {
  if (argc != 7 || !OpaqueId(argv[4]) || HashHex(argv[3]) != argv[4]) return 2;
  (void)setsid();
  SetGracefulShutdown(true);
  std::string bytes, error;
  if (!ReadRegularFile(argv[6], kCommandBytes, bytes, error)) return 2;
  unlink(argv[6]);
  json launch = json::parse(bytes, nullptr, false);
  if (!launch.is_object()) return 2;
  Options options;
  options.yolo = JsonValue(launch, "yolo", false);
  options.debug = JsonValue(launch, "debug", false);
  options.debug_path = JsonValue(launch, "debug_path", "");
  options.trust_project = JsonValue(launch, "trust_project", false);
  if (const json* overrides = JsonObject(launch, "overrides")) {
    for (auto it = overrides->begin(); it != overrides->end(); ++it) {
      if (it.value().is_string()) {
        options.overrides[it.key()] = it.value().get<std::string>();
      }
    }
  }
  Fd owner(JsonValue(launch, "owner_fd", -1));
  WorkerChannel channel(argv[3], argv[4], RandomToken(16), argv[5],
                        static_cast<bool>(owner));
  if (!channel.Start()) return 2;
  if (chdir(argv[2]) != 0) {
    channel.Send({{"kind", "error"}, {"error", "workspace is unavailable"}});
    return 2;
  }
  // Delegated workers belong to the parent session, even after a parent crash.
  // The pipe is close-on-exec in the parent and never reaches tool children.
  Pipe watch_stop;
  std::thread owner_watch;
  if (owner) {
    fcntl(owner.Get(), F_SETFD, FD_CLOEXEC);
    if (!watch_stop.Open()) return 2;
    owner_watch = std::thread([&] {
      pollfd waits[] = {{owner.Get(), POLLIN, 0},
                        {watch_stop.read.Get(), POLLIN, 0}};
      while (poll(waits, 2, -1) < 0 && errno == EINTR) {
      }
      if (waits[0].revents) channel.Close();
    });
  }
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
  watch_stop.Wake();
  if (owner_watch.joinable()) owner_watch.join();
  observation.Unsubscribe(subscriber);
  SetObservability(nullptr);
  return status;
}
}  // namespace uagent::session
