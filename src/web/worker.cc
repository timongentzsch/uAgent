// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/bootstrap.h"
#include "include/cli.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/web/protocol.h"

namespace uagent::web {
namespace {
class WorkerChannel final : public ApplicationChannel {
 public:
  WorkerChannel(std::string path, std::string id, std::string generation,
                std::string title)
      : path_(std::move(path)),
        id_(std::move(id)),
        generation_(std::move(generation)),
        title_(std::move(title)) {}
  ~WorkerChannel() override {
    Close();
    if (reader_.joinable()) {
      reader_.join();
    }
    {
      std::lock_guard lock(output_mutex_);
      finished_ = true;
    }
    output_changed_.notify_all();
    if (writer_.joinable()) {
      writer_.join();
    }
  }

  bool Start() {
    protocol_.Reset(fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3));
    if (!protocol_ || !wake_.Open() || !stop_.Open()) {
      return false;
    }
    fcntl(protocol_.Get(), F_SETFL,
          fcntl(protocol_.Get(), F_GETFL) | O_NONBLOCK);
    Fd null(open("/dev/null", O_WRONLY | O_CLOEXEC));
    if (!null || dup2(null.Get(), STDOUT_FILENO) < 0) {
      return false;
    }
    writer_ = std::thread([this] {
      for (;;) {
        std::string frame;
        {
          std::unique_lock lock(output_mutex_);
          output_changed_.wait(
              lock, [this] { return finished_ || !output_.empty(); });
          if (output_.empty()) {
            return;
          }
          frame = std::move(output_.front());
          output_.pop_front();
          output_bytes_ -= frame.size();
        }
        if (!WriteFrame(protocol_.Get(), std::move(frame))) {
          Close();
          return;
        }
      }
    });
    reader_ = std::thread([this] {
      ReadFrames(STDIN_FILENO, stop_.read.Get(), kCommandBytes,
                 [this](const json& frame) { return Command(frame); });
      Close();
    });
    return true;
  }

  void Event(const AppEvent& event) {
    if (event.type == "message.changed" || event.type == "activities.changed") {
      std::lock_guard lock(mutex_);
      if (event.type == "message.changed") {
        MergeDisplayBlock(state_["view"], event.data["block"]);
      } else {
        state_["activities"] = event.data["activities"];
      }
    }
    if (event.type == "http.exchange") {
      std::lock_guard lock(mutex_);
      state_["http"] = json::array({event.data});
    }
    if (event.type == "config.changed" && event.data.contains("permissions")) {
      std::lock_guard lock(mutex_);
      state_["permissions"] = event.data["permissions"];
      state_["yolo"] = ApprovalIsAutomatic();
    }
    json data = JsonEstimatedBytes(event.data) > kEventBytes
                    ? json{{"truncated", true},
                           {"message",
                            "event exceeds live limit; inspect saved history"}}
                    : event.data;
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
    frame["v"] = kProtocol;
    frame["session_id"] = id_;
    frame["generation"] = generation_;
    std::string line = JsonDump(frame);
    if (JsonValue(frame, "kind", "") == "event" && line.size() > kEventBytes) {
      frame["data"] = {
          {"truncated", true},
          {"message",
           "Live event exceeds preview limit; inspect saved history."}};
      line = JsonDump(frame);
    }
    size_t bytes = line.size();
    std::lock_guard lock(output_mutex_);
    if (finished_ || bytes > kFrameBytes) {
      return;
    }
    if (output_bytes_ + bytes > kQueueBytes || output_.size() >= 1024) {
      output_.clear();
      output_.push_back(JsonDump({{"v", kProtocol},
                                  {"kind", "gap"},
                                  {"session_id", id_},
                                  {"generation", generation_}}));
      output_bytes_ = output_.front().size();
    }
    output_bytes_ += bytes;
    output_.push_back(std::move(line));
    output_changed_.notify_one();
  }

  std::optional<ApplicationInput> NextInput() override {
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        if (closed_) {
          return std::nullopt;
        }
        if (input_) {
          auto result = std::move(input_);
          input_.reset();
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
        if (closed_) {
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
    changed_.wait(lock, [this] {
      return closed_ || reply_.has_value() || AbortRequested();
    });
    std::string answer = reply_.value_or("");
    *eof = closed_ || !reply_;
    reply_.reset();
    pending_.clear();
    decision_ = nullptr;
    SendState();
    return answer;
  }

  int WakeFd() const override { return wake_.write.Get(); }
  std::string SessionPath() const override { return path_; }
  std::string InitialTitle() const override { return title_; }
  void PublishState(const json& state) override {
    std::lock_guard lock(mutex_);
    std::string activity = JsonValue(state_, "activity", "Ready");
    state_ = state;
    state_["activity"] = activity;
    busy_ = input_.has_value();
    if (!busy_) {
      // Completion events precede saved display/HTTP metadata. Only the final
      // application checkpoint makes the turn idle and accepts another input.
      turn_active_ = false;
      state_["activity"] = "Ready";
      ClearAbort();
      NormalizeAbortWake();
      auto queued = SteeringState().TakeMessages();
      if (!queued.empty()) {
        input_ = ApplicationInput{
            .text = std::move(queued.front().text),
            .request_id = std::move(queued.front().request_id)};
        for (size_t i = 1; i < queued.size(); ++i) {
          SteeringState().Queue(std::move(queued[i].text),
                                std::move(queued[i].request_id));
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
          {"checkpoint", checkpoint}});
  }
  void Close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      RequestAbort();
    }
    stop_.Wake();
    wake_.Wake();
    changed_.notify_all();
  }
  bool Command(const json& command) {
    if (JsonValue(command, "session_id", "") != id_ ||
        JsonValue(command, "generation", "") != generation_) {
      return false;
    }
    std::string kind = JsonValue(command, "kind", "");
    std::string request = JsonValue(command, "request_id", "");
    std::string error;
    std::unique_lock lock(mutex_);
    if (!OpaqueId(request)) {
      return false;
    }
    if (kind == "close") {
      lock.unlock();
      Close();
      return false;
    }
    if (kind == "interrupt") {
      RequestAbort();
      changed_.notify_all();
      wake_.Wake();
    } else if (kind == "reply") {
      if (pending_.empty() ||
          JsonValue(command, "interaction_id", "") != pending_ || reply_) {
        error = "decision is stale or already answered";
      } else {
        reply_ = JsonValue(command, "text", "");
        changed_.notify_all();
      }
    } else if (kind == "steer") {
      std::string text = JsonValue(command, "text", "");
      if (!turn_active_ || text.empty() || SteeringState().QueuedCount() >= 8) {
        error = "guidance requires an active turn and space in its queue";
      } else {
        SteeringState().Queue(std::move(text),
                              JsonValue(command, "client_request_id", ""));
        SteeringState().Request();
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
        busy_ = true;
        wake_.Wake();
        SendState();
        return true;  // Completion carries the catalog or validated selection.
      }
    } else if (kind == "submit") {
      // Reuse the one-slot queue while a non-turn control finishes. The
      // application consumes it after publishing that control's checkpoint.
      if (turn_active_ || input_) {
        error = "session is busy";
      } else {
        ApplicationInput input;
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
          error = "use conversation controls or the attachment button";
        }
        if (slash.spec && (slash.spec->id == SlashCommandId::kContext ||
                           slash.spec->id == SlashCommandId::kHttp ||
                           (slash.spec->id == SlashCommandId::kTrace &&
                            !slash.argument.empty()))) {
          error =
              "use Raw context below the composer, HTTP request/response in a "
              "message menu, or Tool input/output";
        }
        if (error.empty()) {
          ClearAbort();
          busy_ = true;
          turn_active_ = !input.text.starts_with("/") ||
                         !SlashCommandPrompt(slash).empty();
          if (turn_active_) BeginTurn();
          input_ = std::move(input);
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
  Fd protocol_;
  Pipe wake_, stop_;
  std::thread reader_, writer_;
  std::mutex mutex_, output_mutex_, control_mutex_;
  std::condition_variable changed_, output_changed_;
  bool closed_ = false, busy_ = true, finished_ = false;
  bool turn_active_ = false;
  std::function<json(const json&)> activity_control_;
  std::optional<ApplicationInput> input_;
  std::optional<std::string> reply_;
  std::string pending_;
  json decision_ = nullptr, state_ = json::object(), approval_ = nullptr;
  std::deque<std::string> output_;
  size_t output_bytes_ = 0;
};
}  // namespace

int WorkerMain(int argc, char** argv) {
  if (argc != 7 || !OpaqueId(argv[4]) || !OpaqueId(argv[5]) ||
      HashHex(argv[3]) != argv[4]) {
    return 2;
  }
  WorkerChannel channel(argv[3], argv[4], argv[5], argv[6]);
  if (!channel.Start()) {
    return 2;
  }
  if (chdir(argv[2]) != 0) {
    channel.Send({{"kind", "error"}, {"error", "workspace is unavailable"}});
    return 2;
  }
  Observability observation;
  SetObservability(&observation);
  observation.EnableTerminal(false);
  observation.EnableJournal(true);
  auto subscriber = observation.Subscribe(
      [&](const AppEvent& event) { channel.Event(event); });
  BootstrapResult boot = Bootstrap(Options{}, argv[0], observation, &channel);
  int status = boot.Ok() ? RunApplication(*boot.context) : boot.exit_code;
  if (!boot.Ok()) {
    channel.Send({{"kind", "error"}, {"error", boot.error}});
  }
  boot.context.reset();
  observation.Unsubscribe(subscriber);
  SetObservability(nullptr);
  return status;
}
}  // namespace uagent::web
