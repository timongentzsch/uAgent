// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/bootstrap.h"
#include "include/app/session.h"
#include "include/cli.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/steering.h"

namespace uagent::session {
namespace {
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
    return wake_.Open() &&
           server_.Start(path_, generation_,
                         [this](const json& frame) { return Command(frame); });
  }

  void Event(const AppEvent& event) {
    {
      std::lock_guard lock(mutex_);
      ApplySessionEvent(state_, event.type, event.data);
      if (event.type == "notice" && !ready_ && notices_.size() < 16) {
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
      phase = "Working";
    } else if (event.type == "response.started") {
      phase = "Waiting for model";
    } else if (event.type == "response.reasoning.delta") {
      phase = "Thinking";
    } else if (event.type == "response.answer.delta") {
      phase = "Responding";
    } else if (event.type == "tool.call") {
      phase = "Running " + JsonValue(data, "name", "tool");
    } else if (event.type == "tool.result") {
      phase = "Working";
    } else if (event.type == "response.hosted_tool") {
      phase = "Searching";
    } else if (event.type == "turn.completed" || event.type == "turn.stopped") {
      phase = "Finishing";
    }
    if (!phase.empty()) {
      std::lock_guard lock(mutex_);
      if (JsonValue(state_, "activity", "") != phase) {
        state_["activity"] = phase;
        Send({{"kind", "activity"},
              {"activity", phase},
              {"busy", turn_active_}});
      }
    }
    Send({{"kind", "event"},
          {"type", event.type},
          {"data", std::move(data)},
          {"time", event.time}});
  }

  void Send(json frame) {
    if (JsonValue(frame, "kind", "") == "outcome") {
      std::lock_guard lock(receipts_mutex_);
      auto found = receipts_.find(JsonValue(frame, "request_id", ""));
      if (found != receipts_.end()) found->second.second = frame;
    }
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
    SendState();
    return answer;
  }

  int WakeFd() const override { return wake_.write.Get(); }
  std::string SessionPath() const override { return path_; }
  std::string InitialTitle() const override { return title_; }
  void PublishState(const json& state, bool checkpoint) override {
    std::lock_guard lock(mutex_);
    std::string activity = JsonValue(state_, "activity", "Ready");
    state_ = state;
    state_["notices"] = notices_;
    state_["activity"] = activity;
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
      ClearAbort();
      NormalizeAbortWake();
      auto queued = SteeringState().TakeAutoStartMessages();
      if (!queued.empty()) {
        input_ = ApplicationInput{
            .text = std::move(queued.front().text),
            .request_id = std::move(queued.front().request_id)};
        for (size_t i = 1; i < queued.size(); ++i) {
          SteeringState().Queue(std::move(queued[i].text),
                                std::move(queued[i].request_id),
                                queued[i].auto_start);
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
      std::lock_guard lock(mutex_);
      closed_ = true;
      RequestAbort();
    }
    wake_.Wake();
  }

 private:
  // A new user or background turn supersedes the preceding turn's stop/error.
  // Publish this transition immediately, before waiting for a model response.
  void BeginTurn() {
    state_["error"] = "";
    state_.erase("stop");
    state_["activity"] = "Working";
  }
  void SendState(bool checkpoint = false) {
    Send({{"kind", "state"},
          {"state", state_},
          {"busy", turn_active_},
          {"command_busy", busy_},
          {"pending", decision_},
          {"guidance", SteeringState().QueuedCount()},
          {"completed_request_id",
           checkpoint ? std::exchange(active_command_, "") : ""},
          {"checkpoint", checkpoint}});
  }
  bool Command(const json& command) {
    if (JsonValue(command, "session_id", "") != id_ ||
        JsonValue(command, "generation", "") != generation_) {
      return false;
    }
    std::string kind = JsonValue(command, "kind", "");
    std::string request = JsonValue(command, "request_id", "");
    std::string error;
    if (!OpaqueId(request)) return false;
    json previous;
    {
      std::lock_guard lock(receipts_mutex_);
      auto found = receipts_.find(request);
      if (found != receipts_.end()) {
        if (found->second.first != command) return false;
        previous = found->second.second;
      } else {
        if (receipts_.size() >= 256) {
          auto completed = std::find_if(
              receipts_.begin(), receipts_.end(), [](const auto& item) {
                return !JsonValue(item.second.second, "pending", false);
              });
          if (completed == receipts_.end()) return false;
          receipts_.erase(completed);
        }
        receipts_[request] = {command,
                              {{"kind", "outcome"},
                               {"request_id", request},
                               {"accepted", true},
                               {"pending", true}}};
      }
    }
    if (!previous.is_null()) {
      Send(std::move(previous));
      return true;
    }
    std::unique_lock lock(mutex_);
    if (kind == "steer" && !turn_active_) kind = "submit";
    if (closed_) return false;
    if (kind == "close") {
      lock.unlock();
      Close();
      return true;
    }
    if (kind == "interrupt") {
      RequestAbort();
      wake_.Wake();
    } else if (kind == "reply") {
      if (pending_.empty() ||
          JsonValue(command, "interaction_id", "") != pending_ || reply_) {
        error = "decision is stale or already answered";
      } else {
        reply_ = JsonValue(command, "text", "");
        reply_cancelled_ = JsonValue(command, "cancelled", false);
        wake_.Wake();
      }
    } else if (kind == "steer" || kind == "guide") {
      std::string text = JsonValue(command, "text", "");
      if (!turn_active_ || text.empty() || SteeringState().QueuedCount() >= 8) {
        error = "guidance requires an active turn and space in its queue";
      } else {
        // Guidance only: the turn reads it at its next steering check and
        // passive waits yield on the queued message. Requesting a foreground
        // abort here would report every steer as an interruption.
        SteeringState().Queue(std::move(text),
                              JsonValue(command, "client_request_id", ""),
                              kind != "guide");
      }
    } else if (kind == "recall") {
      // Pre-delivery only: the queue owns recallability, the turn owns
      // delivery. No match means the turn already took it.
      if (!SteeringState().Recall(
              JsonValue(command, "client_request_id", ""))) {
        error = "already delivered";
      }
    } else if (kind == "rename") {
      std::string title = JsonValue(command, "title", "");
      if (input_ || !ValidSessionTitle(title)) {
        error = "rename requires a valid title and an empty input queue";
      } else {
        input_ = ApplicationInput{.title = std::move(title)};
        busy_ = true;
        wake_.Wake();
        SendState();
      }
    } else if (kind == "refresh") {
      SendState();
    } else if (kind == "permissions" ||
               (kind == "activity" &&
                JsonValue(command, "operation", "") != "followup")) {
      lock.unlock();
      std::lock_guard control(control_mutex_);
      CompleteControl(request, activity_control_
                                   ? activity_control_(command)
                                   : json{{"error", "session not ready"}});
      return true;
    } else if (kind == "model" || kind == "activity" || kind == "config" ||
               kind == "context" || kind == "fork" || kind == "prompt") {
      // Reuse the one-slot queue while a preceding non-turn control finishes.
      if (turn_active_ || input_) {
        error = "this control requires an idle session";
      } else {
        input_ = ApplicationInput{.request_id = request, .control = command};
        input_command_ = request;
        busy_ = true;
        wake_.Wake();
        SendState();
        return true;  // Completion carries the catalog or validated selection.
      }
    } else if (kind == "submit" && turn_active_ &&
               !JsonValue(command, "text", "").empty() &&
               !JsonValue(command, "text", "").starts_with("/") &&
               !command.contains("attachments")) {
      if (SteeringState().QueuedCount() >= 8) {
        error = "guidance queue is full";
      } else {
        SteeringState().Queue(JsonValue(command, "text", ""),
                              JsonValue(command, "client_request_id", request));
      }
      SendState();
    } else if (kind == "submit") {
      // Reuse the one-slot queue while a non-turn control finishes. The
      // application consumes it after publishing that control's checkpoint.
      if (turn_active_ || input_) {
        error = "session is busy";
      } else {
        ApplicationInput input;
        if (delegated_) {
          input.budget = JsonValue(command, "budget", json{});
        }
        input.request_id = JsonValue(command, "client_request_id", "");
        input.text = JsonValue(command, "text", "");
        const json* paths = JsonArray(command, "attachments");
        if (paths && paths->size() <= kUploadCount) {
          for (const json& path : *paths) {
            Attachment attachment;
            std::string file = path.is_string() ? path.get<std::string>()
                                                : JsonValue(path, "path", "");
            if (!InspectAttachment(file, attachment, error)) {
              break;
            }
            attachment.asset_id =
                std::filesystem::path(attachment.path).stem().string();
            if (path.is_object()) {
              attachment.asset_id = JsonValue(path, "id", "");
              attachment.name = JsonValue(path, "name", attachment.name);
              attachment.mime = JsonValue(path, "mime", attachment.mime);
              attachment.image = JsonValue(path, "image", false);
            }
            input.attachments.push_back(std::move(attachment));
          }
        } else if (paths) {
          error = "too many attachments";
        }
        if (input.text.empty() && input.attachments.empty()) {
          error = "empty message";
        }
        ParsedSlashCommand slash = ParseSlashCommand(input.text);
        if (slash.spec && (slash.spec->id == SlashCommandId::kReset ||
                           slash.spec->id == SlashCommandId::kFork ||
                           slash.spec->id == SlashCommandId::kSessions ||
                           slash.spec->id == SlashCommandId::kQuit)) {
          error = "use conversation controls to navigate, fork, or close";
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
    } else {
      error = "unsupported command";
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
  std::mutex receipts_mutex_;
  std::map<std::string, std::pair<json, json>> receipts_;
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
