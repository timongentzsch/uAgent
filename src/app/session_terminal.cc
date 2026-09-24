// Copyright 2026 Timon Gentzsch
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/session.h"
#include "include/cli.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/ui/display.h"
#include "include/ui/editor.h"
#include "include/ui/interactive.h"
#include "include/ui/presentation.h"
#include "include/ui/sessions.h"

namespace uagent::session {
namespace {
// The pinned row from the worker's state frame: the working row while a turn
// runs, the session row otherwise -- the same renderers presentation_test pins.
std::string StatusRow(const json& state,
                      std::chrono::steady_clock::duration elapsed,
                      bool interrupting) {
  const int64_t used = JsonValue(state, "context_tokens", int64_t{0});
  const int64_t window = JsonValue(state, "context_window", int64_t{0});
  const std::string route = JsonValue(state, "route", "");
  size_t background = 0, foreground = 0, subagents = 0;
  std::string subagent;
  if (const json* rows = JsonArray(state, "activities")) {
    for (const json& row : *rows) {
      if (JsonValue(row, "status", "") != "running") continue;
      if (JsonValue(row, "kind", "") == "agent") {
        ++subagents;
        std::string progress = JsonValue(row, "progress", "");
        if (!progress.empty()) {
          subagent = JsonValue(row, "id", "") + ": " + progress;
        }
      } else if (JsonValue(row, "detached", false)) {
        ++background;
      } else {
        ++foreground;
      }
    }
  }
  if (JsonValue(state, "turn_active", false)) {
    return ActivityBar({.elapsed = elapsed,
                        .context_used = used,
                        .context_window = window,
                        .model = route,
                        .background = background,
                        .foreground = foreground,
                        .subagents = subagents,
                        .interrupting = interrupting,
                        .subagent = std::move(subagent)});
  }
  const json permissions = JsonValue(state, "permissions", json::object());
  return StatusBar(UsageFromJson(JsonValue(state, "usage", json::object())),
                   {.activity = JsonValue(state, "activity", "Connecting"),
                    .context_used = used,
                    .context_window = window,
                    .model = route,
                    .approval = JsonValue(permissions, "effective", "ask"),
                    .background = background + subagents});
}

class Terminal {
 public:
  Terminal(Connection connection, std::string path)
      : connection_(std::move(connection)),
        path_(std::move(path)),
        composer_(output_) {}
  int Run(const std::vector<std::string>& attachments) {
    if (!stop_.Open() || !wake_.Open()) return 1;
    raw_ = isatty(STDIN_FILENO) && output_.Start() && composer_.Start();
    if (!raw_) output_.Stop();
    SetPersistentComposer(raw_);
    SetTerminalWakeFd(wake_.write.Get());
    reader_ = std::thread([this] {
      ReadFrames(connection_.socket.Get(), stop_.read.Get(), kFrameBytes,
                 [this](const json& frame) {
                   Receive(frame);
                   return true;
                 });
      presenter_.Finish();
      if (!detaching_ && !ended_) {
        failed_ = true;
        WriteTerminalRecord(
            "Session connection closed; use -c to reconnect.\n");
      }
      disconnected_ = true;
      wake_.Wake();
    });
    if (raw_) {
      output_.Write("Connecting\n");
      composer_.Mount(InputPrompt());
    }
    std::vector<std::string> files = attachments;
    std::string cooked;
    bool quit = false;
    int exit_code = 0;
    while (!quit && !disconnected_ && !navigate_) {
      bool running;
      {
        std::lock_guard lock(mutex_);
        running = running_;
      }
      g_streaming = running ? 1 : 0;
      SetQuitGesture(true);
      pollfd waits[] = {
          {!raw_ && input_blocked_ && !interaction_ ? -1 : STDIN_FILENO, POLLIN,
           0},
          {raw_ ? output_.ReadFd() : -1, POLLIN, 0},
          {wake_.read.Get(), POLLIN, 0},
          {AbortWakeFd(), POLLIN, 0}};
      int timeout = -1;
      if (raw_ && composer_.WakeDeadline()) {
        timeout = PollTimeoutMs(*composer_.WakeDeadline());
      }
      // The working row's spinner and elapsed time advance on their own.
      if (raw_ && running) timeout = timeout < 0 ? 100 : std::min(timeout, 100);
      if (poll(waits, 4, timeout) < 0 && errno != EINTR) break;
      wake_.Drain();
      if (TakeIdleInterrupt()) {
        if (quit_hint_) {
          exit_code = 130;
          break;
        }
        quit_hint_ = true;
        if (raw_) {
          Unmount();
          composer_.Clear();
        }
        output_.Write("Press Ctrl+C again to detach.\n");
        if (raw_) {
          output_.Write(StatusBarLine(last_status_, &status_columns_) + "\n");
          composer_.Remount();
        }
      }
      if (AbortRequested()) {
        Send({{"kind", "interrupt"}});
        interrupting_ = true;
        ClearAbort();
        NormalizeAbortWake();
      }
      if (raw_) Paint();
      json pending;
      {
        std::lock_guard lock(mutex_);
        pending = pending_;
        running = running_;
      }
      std::string decision = JsonValue(pending, "id", "");
      if (decision != decision_) {
        decision_ = decision;
        if (!decision.empty() && JsonValue(pending, "kind", "") == "editor") {
          std::string text = JsonValue(pending, "initial", "");
          if (raw_) Unmount();
          bool edited =
              raw_ ? composer_.EditTextExternally(text)
                   : EditExternalText(text, STDIN_FILENO, kAdaptiveSystemBytes);
          Send({{"kind", "reply"},
                {"interaction_id", decision},
                {"text", text},
                {"cancelled", !edited}});
          if (raw_) composer_.Remount();
          continue;
        }
        if (!decision.empty()) {
          std::string description;
          if (const json* approval = JsonObject(pending, "approval")) {
            description = JsonValue(*approval, "mandatory_reason", "") + "\n" +
                          JsonValue(*approval, "preview", "") + "\n";
          }
          if (const json* options = JsonArray(pending, "options")) {
            for (const auto& option : *options) {
              if (option.is_object()) {
                description += JsonValue(option, "value", "") + " · " +
                               JsonValue(option, "label", "") + "\n";
              }
            }
          }
          if (raw_) {
            Unmount();
            output_.Write(ColorizeDiffLines(TerminalSafe(description)));
            composer_.Remount();
          } else {
            fputs(TerminalSafe(description).c_str(), stdout);
          }
        }
        if (raw_) {
          Unmount();
          if (!decision.empty()) {
            draft_ = composer_.Buffer();
            output_.Write(TerminalSafe(JsonValue(pending, "prompt", "")) +
                          "\n");
            composer_.Mount("> ", JsonValue(pending, "initial", ""), false);
          } else {
            composer_.Mount(InputPrompt(), draft_);
          }
        } else if (!decision.empty()) {
          printf("%s\n",
                 TerminalSafe(JsonValue(pending, "prompt", "")).c_str());
        }
      }
      if (!(waits[0].revents & (POLLIN | POLLHUP)) &&
          !(raw_ && (composer_.HasPending() || composer_.WakeDeadline()))) {
        continue;
      }
      InteractiveInputEvent input;
      if (raw_) {
        input = composer_.Read();
      } else {
        char byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count <= 0) break;
        if (byte != '\n') {
          cooked += byte;
          continue;
        }
        input = {InteractiveInputKind::kLine, std::move(cooked)};
        cooked.clear();
      }
      if (input.kind == InteractiveInputKind::kNone) continue;
      quit_hint_ = false;
      if (input.kind == InteractiveInputKind::kEof) {
        quit = true;
        continue;
      }
      if (input.kind == InteractiveInputKind::kEscape) {
        if (raw_) composer_.Clear();
        draft_.clear();
        if (!decision.empty()) {
          Send({{"kind", "reply"}, {"interaction_id", decision}, {"text", ""}});
        }
        if (running) {
          Send({{"kind", "interrupt"}});
          interrupting_ = true;
        }
        continue;
      }
      if (input.kind == InteractiveInputKind::kBackground) {
        Send({{"kind", "activity"}, {"operation", "background"}});
        continue;
      }
      if (input.kind != InteractiveInputKind::kLine) continue;
      if (raw_) {
        output_.Write("\r\033[" +
                      std::to_string(composer_.LastSubmittedRows() + 1) +
                      "A\033[J");
        if (decision.empty() && !Trim(input.text).empty()) {
          output_.Write(UserEchoRow(InputPrompt(), TerminalSafe(input.text)) +
                        "\n");
          if (!tail_.empty()) {
            output_.AdoptTail();
            tail_.clear();
          }
        }
        output_.Write(StatusBarLine(last_status_, &status_columns_) + "\n");
        composer_.Mount(InputPrompt());
      }
      std::string text = Trim(input.text);
      const ParsedSlashCommand slash = ParseSlashCommand(text);
      if (slash.spec && slash.spec->id == SlashCommandId::kQuit) break;
      if (text == "/clear" || text.starts_with("/clear ")) {
        // Screen only: the session keeps running underneath.
        if (raw_) {
          output_.Write("\033[H\033[2J");
        } else {
          printf("\033[H\033[2J");
          fflush(stdout);
        }
        continue;
      }
      if (text == "/reset" || text == "/new" || text == "/sessions" ||
          text.starts_with("/sessions ") || text == "/resume" ||
          text.starts_with("/resume ")) {
        // Guarded switch: never abandon a running turn by accident.
        {
          std::lock_guard lock(mutex_);
          if (!waiting_.empty() || running_) {
            WriteTerminalRecord(
                "· turn active; interrupt it before switching sessions\n");
            continue;
          }
        }
        std::string target = "/reset";
        if (text != "/reset" && text != "/new") {
          std::string arg;
          if (text.starts_with("/sessions ")) {
            arg = Trim(text.substr(10));
          } else if (text.starts_with("/resume ")) {
            arg = Trim(text.substr(8));
          }
          std::string matched = arg.empty() ? "" : MatchSessionPrefix(arg);
          if (!arg.empty() && matched.empty()) {
            WriteTerminalRecord("· no unique session matches \"" +
                                TerminalSafe(arg) + "\"\n");
            continue;
          }
          if (matched.empty()) {
            matched = PickSession();
            if (matched.empty()) continue;
          }
          target = matched;
        }
        std::lock_guard lock(mutex_);
        next_ = target;
        break;
      }
      if (!decision.empty()) {
        Send({{"kind", "reply"}, {"interaction_id", decision}, {"text", text}});
      } else if (text.starts_with("/attach ")) {
        // Terminals quote dropped paths containing spaces; strip one
        // surrounding pair so a drop Just Works.
        std::string file = Unquote(Trim(text.substr(8)));
        if (file == "clear") {
          files.clear();
        } else {
          files.push_back(CanonicalAccessPath(file).string());
        }
      } else if (text == "/fork" || text.starts_with("/fork ")) {
        const ForkArgument fork = ParseForkArgument(text.substr(5));
        Send({{"kind", "fork"}, {"title", fork.title}, {"turn", fork.turn}});
      } else if (text == "/rewind" || text.starts_with("/rewind ")) {
        // Same [@]N grammar as /fork's turn suffix, title aside: rewind
        // truncates this session in place instead of branching it.
        std::string rest = Trim(text.substr(7));
        if (rest.starts_with("@")) rest = Trim(rest.substr(1));
        int64_t turn = 0;
        if (!rest.empty() && rest.size() <= 9 &&
            std::all_of(rest.begin(), rest.end(), ::isdigit)) {
          turn = std::stoll(rest);
        }
        Send({{"kind", "rewind"}, {"turn", turn}});
      } else if (text == "/share") {
        Send({{"kind", "share"}});
      } else if (text.starts_with("/share ")) {
        WriteTerminalRecord("· usage: /share\n");
        continue;
      } else if (!text.empty() || !files.empty()) {
        json command = {{"kind", "submit"}, {"text", text}};
        if (!files.empty()) command["attachments"] = files;
        Send(std::move(command));
        files.clear();
      }
    }
    detaching_ = true;
    if (!files.empty()) {
      // Staged via /attach but never submitted (EOF/quit/compose-cancel):
      // say so instead of dropping them silently.
      std::string dropped = "· " + std::to_string(files.size()) +
                            " staged attachment" +
                            (files.size() == 1 ? "" : "s") +
                            " discarded: nothing was submitted\n";
      if (raw_) {
        output_.Write("\r" + TerminalSafe(dropped));
      } else {
        fputs(TerminalSafe(dropped).c_str(), stdout);
      }
    }
    stop_.Wake();
    if (reader_.joinable()) {
      // Drain output while the presenter finishes; joining with a full pipe
      // would deadlock a client leaving during a long response.
      while (!disconnected_) {
        if (raw_) Paint();
        pollfd ready{wake_.read.Get(), POLLIN, 0};
        poll(&ready, 1, 20);
      }
      reader_.join();
    }
    if (raw_) {
      Paint();
      Unmount();
      composer_.Stop();
      output_.Stop();
    }
    SetTerminalWakeFd(-1);
    SetPersistentComposer(false);
    SetQuitGesture(false);
    g_streaming = 0;
    return exit_code ? exit_code : failed_ ? 1 : 0;
  }
  const std::string& Next() const { return next_; }

 private:
  void Send(json command) {
    std::lock_guard lock(send_mutex_);
    if (JsonValue(command, "kind", "") == "reply") interaction_ = false;
    command["v"] = kProtocol;
    command["session_id"] = HashHex(path_);
    command["generation"] = connection_.generation;
    command["request_id"] = RandomToken(16);
    command["client_request_id"] = command["request_id"];
    {
      std::lock_guard state_lock(mutex_);
      own_requests_.insert(command["request_id"]);
      if (JsonValue(command, "kind", "") == "submit" ||
          JsonValue(command, "kind", "") == "fork" ||
          JsonValue(command, "kind", "") == "rewind" ||
          JsonValue(command, "kind", "") == "share") {
        if (raw_ && JsonValue(command, "kind", "") == "submit" &&
            !JsonValue(command, "text", "").starts_with('/')) {
          echoed_.insert(command["request_id"]);
        }
        waiting_ = command["request_id"];
        input_blocked_ = true;
      }
    }
    if (!WriteFrame(connection_.socket.Get(), command)) {
      failed_ = true;
      stop_.Wake();
    }
  }
  void Receive(const json& frame) {
    auto kind = JsonValue(frame, "kind", "");
    if (kind == "state") {
      const json state = JsonValue(frame, "state", json::object());
      {
        std::lock_guard lock(mutex_);
        for (const char* field :
             {"activity", "route", "usage", "turn_active", "context_tokens",
              "context_window", "activities", "permissions"}) {
          if (state.contains(field)) state_[field] = state[field];
        }
        pending_ = JsonValue(frame, "pending", json(nullptr));
        running_ = JsonValue(frame, "busy", false);
        interaction_ = !pending_.is_null();
        if (JsonValue(frame, "checkpoint", false) &&
            (waiting_.empty() ||
             JsonValue(frame, "completed_request_id", "") == waiting_)) {
          waiting_.clear();
          input_blocked_ = JsonValue(frame, "command_busy", false);
        }
      }
      if (!history_) {
        if (const json* notices = JsonArray(state, "notices")) {
          for (const auto& notice : *notices) {
            presenter_.Consume(AppEvent{0, "", "notice", notice, false});
          }
        }
        const json view = JsonValue(state, "view", json::object());
        if (const json* blocks = JsonArray(view, "blocks")) {
          for (const auto& block : *blocks) {
            shown_.insert(JsonValue(block, "id", ""));
            presenter_.Block(block);
          }
          history_ = true;
        }
      }
      wake_.Wake();
    } else if (kind == "activity") {
      presenter_.Consume(AppEvent{0, "", "activity.status", frame, false});
      std::lock_guard lock(mutex_);
      state_["activity"] = JsonValue(frame, "activity", "Ready");
      wake_.Wake();
    } else if (kind == "event") {
      std::string type = JsonValue(frame, "type", "");
      json data = JsonValue(frame, "data", json::object());
      if (type == "session.ended") ended_ = true;
      if (type == "notice" && !history_) return;
      if (type == "usage.updated") {
        std::lock_guard lock(mutex_);
        ApplySessionEvent(state_, type, data);
        wake_.Wake();
      } else if (type == "message.changed") {
        json block = JsonValue(data, "block", json::object());
        const auto block_kind = JsonValue(block, "kind", "");
        bool echoed = false;
        {
          std::lock_guard lock(mutex_);
          echoed = echoed_.erase(JsonValue(block, "request_id", "")) > 0;
        }
        if (shown_.insert(JsonValue(block, "id", "")).second && !echoed &&
            (block_kind == "user" || block_kind == "compaction")) {
          presenter_.Block(block);
        }
      } else if (type == "command.completed" && data.contains("output")) {
        std::string output = JsonValue(data, "output", "");
        if (output.empty() && data.contains("result") &&
            !data["result"].empty()) {
          output = JsonDump(data["result"], 2);
        }
        if (!output.empty()) WriteTerminalRecord(TerminalSafe(output) + "\n");
      } else {
        presenter_.Consume(
            AppEvent{0, JsonValue(frame, "time", ""), type, data, false});
      }
    } else if (kind == "outcome") {
      {
        std::lock_guard lock(mutex_);
        if (JsonValue(frame, "pending", false) ||
            !own_requests_.erase(JsonValue(frame, "request_id", ""))) {
          return;
        }
      }
      auto error = JsonValue(frame, "error", "");
      if (!error.empty()) {
        input_blocked_ = false;
        WriteTerminalRecord(TerminalSafe(error) + "\n");
      }
      const json result = JsonValue(frame, "result", json::object());
      if (!result.empty()) {
        WriteTerminalRecord(TerminalSafe(JsonDump(result, 2)) + "\n");
      }
      if (JsonValue(result, "forked", false)) {
        std::lock_guard lock(mutex_);
        next_ = JsonValue(result, "path", "");
        navigate_ = true;
        wake_.Wake();
      }
      if (JsonValue(result, "rewound", false)) {
        // Same worker, truncated transcript: resync the view in place.
        Send({{"kind", "refresh"}});
      }
    } else if (kind == "error") {
      failed_ = true;
      WriteTerminalRecord(TerminalSafe(JsonValue(frame, "error", "")) + "\n");
    } else if (kind == "gap") {
      Send({{"kind", "refresh"}});
    }
    fflush(stdout);
    wake_.Wake();
  }
  void Unmount() {
    if (!composer_.Drawn()) return;
    output_.Write(
        "\r\033[" +
        std::to_string(composer_.CaretRow() + 1 +
                       DisplayRows(tail_, TerminalWidth()) +
                       StatusOverflowRows(status_columns_, TerminalWidth())) +
        "A\033[J");
    composer_.Detach();
  }
  void Paint() {
    auto update = output_.Read();
    if (update.adopted_prefix_bytes) {
      tail_.clear();
      update.committed.erase(0, update.adopted_prefix_bytes);
      update.changed = !update.committed.empty() || !update.tail.empty();
    }
    json state;
    {
      std::lock_guard lock(mutex_);
      state = state_;
    }
    if (!JsonValue(state, "turn_active", false)) {
      turn_started_.reset();
      interrupting_ = false;
    } else if (!turn_started_) {
      turn_started_ = std::chrono::steady_clock::now();
    }
    std::string status = StatusRow(
        state,
        turn_started_ ? std::chrono::steady_clock::now() - *turn_started_
                      : std::chrono::steady_clock::duration{},
        interrupting_);
    const bool resized = g_terminal_resized != 0;
    g_terminal_resized = 0;
    if (!update.changed && !resized && composer_.Drawn()) {
      if (status == last_status_) return;
      const size_t rows = composer_.CaretRow() + 1;
      output_.Write(
          "\r\033[" + std::to_string(rows) + "A" +
          StatusBarLine(status, &status_columns_) + "\033[" +
          std::to_string(rows) + "B\r" +
          (composer_.CaretColumn()
               ? "\033[" + std::to_string(composer_.CaretColumn()) + "C"
               : ""));
    } else {
      Unmount();
      if (update.changed) {
        output_.Write(update.committed);
        tail_ = std::move(update.tail);
      }
      if (!tail_.empty()) output_.Write(tail_ + "\n");
      output_.Write(StatusBarLine(status, &status_columns_) + "\n");
      composer_.Remount();
    }
    last_status_ = std::move(status);
  }

  Connection connection_;
  std::string path_, decision_, draft_, tail_, waiting_, next_, last_status_;
  InteractiveOutput output_;
  RawComposer composer_;
  TerminalPresenter presenter_;
  Pipe stop_, wake_;
  std::thread reader_;
  std::mutex mutex_, send_mutex_;
  json state_ = json::object(), pending_;
  bool running_ = false, history_ = false, raw_ = false;
  bool quit_hint_ = false;
  size_t status_columns_ = 0;
  std::optional<std::chrono::steady_clock::time_point> turn_started_;
  std::atomic<bool> interrupting_{false};
  std::atomic<bool> disconnected_{false}, detaching_{false}, ended_{false},
      failed_{false};
  std::atomic<bool> navigate_{false};
  std::atomic<bool> input_blocked_{true}, interaction_{false};
  std::set<std::string> shown_;
  std::set<std::string> own_requests_, echoed_;
};
}  // namespace
int TerminalMain(Options options) {
  std::string path;
  if (options.resume_pick) {
    path = PickSession();
  } else if (options.resume_latest) {
    auto sessions = ListSessions();
    if (!sessions.empty()) path = sessions.front().path;
  }
  for (;;) {
    std::string cwd = CanonicalCwd();
    if (!path.empty()) {
      for (const auto& item : ListSessions(SessionScope::kAll)) {
        if (item.path == path) {
          cwd = item.cwd;
          break;
        }
      }
    }
    if (path.empty()) {
      path = UagentDir(kHistoryDir) + "/" + WorkspaceId(cwd) + "/" +
             MakeSessionId() + ".json";
    }
    std::string error;
    auto connection = Open(ExecutablePath(), cwd, path, "", options, error);
    if (!connection.socket) {
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    Terminal terminal(std::move(connection), path);
    int result = terminal.Run(options.attach_paths);
    options.attach_paths.clear();
    if (result || terminal.Next().empty()) return result;
    if (terminal.Next() == "/reset") {
      path.clear();
    } else {
      path = terminal.Next();
    }
  }
}
}  // namespace uagent::session
