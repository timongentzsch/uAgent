// Copyright 2026 Timon Gentzsch
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/coordinator.h"
#include "include/app/session.h"
#include "include/cli.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/ui/ask_picker.h"
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
                      bool interrupting, bool verbose) {
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
                    .verbose = verbose,
                    .background = background + subagents});
}

// The SGR that restores what `text` leaves set: a row repainted after the
// status row's reset keeps the bold or colour an earlier row opened. Each
// attribute keeps only its latest setting, so the sequence stays short however
// many spans the paragraph toggled.
std::string ActiveSgr(std::string_view text) {
  std::map<int, std::string_view> active;  // attribute -> the code that set it
  for (size_t at = text.find("\033["); at != std::string_view::npos;
       at = text.find("\033[", at + 1)) {
    const size_t end = text.find_first_not_of("0123456789;", at + 2);
    if (end == std::string_view::npos || text[end] != 'm') continue;
    const std::string_view codes = text.substr(at + 2, end - at - 2);
    for (size_t from = 0; from <= codes.size();) {
      const std::string_view spelled =
          codes.substr(from, codes.find(';', from) - from);
      from += spelled.size() + 1;
      int code = 0;  // an empty parameter is a reset
      std::from_chars(spelled.data(), spelled.data() + spelled.size(), code);
      if (code == 0) {
        active.clear();
      } else if (code == 22) {
        active.erase(1);
        active.erase(2);
      } else if (code >= 23 && code <= 29) {
        active.erase(code - 20);
      } else if (code <= 9) {
        active[code] = spelled;
      } else if (code == 39 || code == 49) {
        active.erase(code);
      } else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
        active[39] = spelled;
      } else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) {
        active[49] = spelled;
      } else {
        break;  // e.g. 38;5;n: what follows are its arguments, not codes
      }
    }
  }
  std::string sequence;
  for (const auto& [attribute, spelled] : active) {
    if (!sequence.empty()) sequence += ';';
    sequence += spelled;
  }
  return sequence.empty() ? sequence : "\033[" + sequence + "m";
}

class Terminal {
 public:
  // `draft` starts the composer, e.g. the message a rewind forked before.
  Terminal(Connection connection, std::string path, std::string draft = "")
      : connection_(std::move(connection)),
        path_(std::move(path)),
        draft_(std::move(draft)),
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
      composer_.Mount(InputPrompt(), draft_);
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
      if (const auto deadline = composer_.WakeDeadline(); raw_ && deadline) {
        timeout = PollTimeoutMs(*deadline);
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
        if (!decision.empty() && JsonValue(pending, "kind", "") == "ask" &&
            raw_) {
          Unmount();
          const json answered = PickAskAnswers(
              JsonValue(pending, "questions", json::array()),
              [this](const std::string& text) { output_.Write(text); },
              [this, decision] {
                std::lock_guard lock(mutex_);
                return JsonValue(pending_, "id", "") == decision;
              });
          // Answered elsewhere meanwhile: nothing left to send.
          bool live = false;
          {
            std::lock_guard lock(mutex_);
            live = JsonValue(pending_, "id", "") == decision;
          }
          if (live) {
            Send(answered.is_null()
                     ? json{{"kind", "reply"},
                            {"interaction_id", decision},
                            {"text", ""},
                            {"cancelled", true}}
                     : json{{"kind", "reply"},
                            {"interaction_id", decision},
                            {"text", answered["text"]},
                            {"attachments", answered["attachments"]}});
          }
          output_.Write("\n");
          composer_.Remount();
          continue;
        }
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
          if (const json* questions = JsonArray(pending, "questions")) {
            for (const json& question : *questions) {
              description += JsonValue(question, "question", "") + "\n";
              size_t number = 0;
              for (const json& option : question["options"]) {
                description += "  " + std::to_string(++number) + ". " +
                               JsonValue(option, "label", "") + "\n";
              }
            }
            description +=
                "Answer with option numbers or your own words; separate "
                "questions with ;\n";
          }
          if (raw_) {
            Unmount();
            output_.Write(ColorizeDiffLines(TerminalSafe(description)));
            composer_.Remount();
          } else {
            fputs(TerminalSafe(description).c_str(), stdout);
          }
        }
        const std::string prompt = TerminalSafe(
            DecisionPrompt(JsonValue(pending, "prompt", ""),
                           JsonValue(pending, "options", json::array())));
        if (raw_) {
          Unmount();
          if (!decision.empty()) {
            draft_ = composer_.Buffer();
            output_.Write(prompt + "\n");
            composer_.Mount(InputPrompt(), JsonValue(pending, "initial", ""),
                            false);
          } else {
            composer_.Mount(InputPrompt(), draft_);
          }
        } else if (!decision.empty()) {
          printf("%s\n", prompt.c_str());
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
        output_.Write("\r" + CursorUp(composer_.LastSubmittedRows() + 1) +
                      EraseBelow());
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
      const Command handled =
          HandleCommand(ParseSlashCommand(text), !decision.empty(), files);
      if (handled == Command::kLeave) break;
      if (handled == Command::kDone) continue;
      if (!decision.empty()) {
        // Without raw input an ask is answered on one line.
        if (JsonValue(pending, "kind", "") == "ask") {
          text = AskAnswersFromLine(
              JsonValue(pending, "questions", json::array()), text);
        }
        Send({{"kind", "reply"}, {"interaction_id", decision}, {"text", text}});
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
      const std::string dropped =
          Note(Tone::kNeutral, std::to_string(files.size()) +
                                   " staged attachment" +
                                   (files.size() == 1 ? "" : "s") +
                                   " discarded: nothing was submitted");
      if (raw_) {
        output_.Write("\r" + dropped);
      } else {
        fputs(dropped.c_str(), stdout);
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
  // Where Next() opens when it has no file yet (a folder's first coordinator).
  const std::string& NextFolder() const { return next_folder_; }
  // The folder whose coordinator this session answers to: a thread's
  // project, else the session's own folder.
  std::string Folder() const {
    const json header = SessionHeader(path_);
    const std::string folder = JsonValue(
        JsonValue(header, kSessionHeaderThread, json::object()), "folder", "");
    if (!folder.empty()) return folder;
    return JsonValue(header, kSessionHeaderCwd, CanonicalCwd());
  }
  // A /board id prefix or a unique title match among the folder's sessions.
  std::string MatchFolderSession(const std::string& argument) const {
    const std::string wanted = AsciiLower(Trim(argument));
    if (wanted.empty()) return "";
    std::string match;
    for (const SessionInfo& info : FolderSessions(Folder())) {
      if (!HashHex(info.path).starts_with(wanted) &&
          AsciiLower(info.title).find(wanted) == std::string::npos) {
        continue;
      }
      if (!match.empty()) return "";
      match = info.path;
    }
    return match;
  }
  // What the next session's composer starts with.
  const std::string& Carry() const { return carry_; }

 private:
  // What a line did: nothing here (it goes on as the reply or to the
  // runtime), all of it, or ended this session's input loop.
  enum class Command { kPass, kDone, kLeave };

  // The slash commands this client performs itself. Leaving the session
  // works while a decision is pending; the rest is then the reply's text.
  Command HandleCommand(const ParsedSlashCommand& slash, bool deciding,
                        std::vector<std::string>& files) {
    if (!slash.spec) return Command::kPass;
    const std::string& argument = slash.argument;
    switch (slash.spec->id) {
      case SlashCommandId::kQuit:
        return Command::kLeave;
      case SlashCommandId::kClear:
        // Screen only: the session keeps running underneath.
        if (raw_) {
          output_.Write(ClearScreen());
        } else {
          fputs(ClearScreen(), stdout);
          fflush(stdout);
        }
        return Command::kDone;
      case SlashCommandId::kBoard:
        WriteTerminalRecord(TerminalSafe(CoordinatorBoard(Folder())));
        return Command::kDone;
      case SlashCommandId::kCoord:
      case SlashCommandId::kOpen: {
        std::string target;
        if (slash.spec->id == SlashCommandId::kCoord) {
          next_folder_ = Folder();
          target = CoordinatorPath(next_folder_);
        } else if (target = MatchFolderSession(argument); target.empty()) {
          WriteTerminalRecord(
              Note(Tone::kNeutral, "no unique session in /board matches \"" +
                                       TerminalSafe(argument) + "\""));
          return Command::kDone;
        }
        return Leave(target, "switching sessions");
      }
      case SlashCommandId::kRestart:
        // A fresh runtime for this conversation, e.g. after a setting that
        // needs a restart; its history is kept.
        if (Leave("/restart", "restarting") == Command::kDone) {
          return Command::kDone;
        }
        Send({{"kind", "close"}});
        return Command::kLeave;
      case SlashCommandId::kReset:
        return Leave("/reset", "switching sessions");
      case SlashCommandId::kSessions: {
        // Guarded before the picker: never abandon a running turn.
        if (TurnActive("switching sessions")) return Command::kDone;
        std::string target = argument.empty() ? PickSession()
                                              : MatchSessionPrefix(argument);
        if (!argument.empty() && target.empty()) {
          WriteTerminalRecord(Note(Tone::kNeutral,
                                   "no unique session matches \"" +
                                       TerminalSafe(argument) + "\""));
        }
        if (target.empty()) return Command::kDone;
        return Leave(target, "switching sessions");
      }
      default:
        break;
    }
    if (deciding) return Command::kPass;
    switch (slash.spec->id) {
      case SlashCommandId::kAttach:
        // Bare /attach lists them (host). Terminals quote dropped paths
        // containing spaces; strip one surrounding pair so a drop Just Works.
        if (argument.empty()) return Command::kPass;
        if (Unquote(argument) == "clear") {
          files.clear();
        } else {
          files.push_back(CanonicalAccessPath(Unquote(argument)).string());
        }
        return Command::kDone;
      case SlashCommandId::kFork: {
        const ForkArgument fork = ParseForkArgument(argument);
        Send({{"kind", "fork"}, {"title", fork.title}, {"turn", fork.turn}});
        return Command::kDone;
      }
      case SlashCommandId::kRewind: {
        // Forks before message N and opens the fork with that message to
        // edit; the original stays. Bare /rewind lists the numbers (host).
        if (argument.empty()) return Command::kPass;
        const ForkArgument parsed = ParseForkArgument(argument);
        if (parsed.turn <= 0 || !parsed.title.empty()) {
          WriteTerminalRecord(Note(
              Tone::kNeutral, "usage: /rewind N (bare /rewind lists them)"));
          return Command::kDone;
        }
        rewinding_ = true;
        Send({{"kind", "fork"}, {"turn", parsed.turn}});
        return Command::kDone;
      }
      case SlashCommandId::kBtw:
        if (argument.empty()) return Command::kPass;
        Send({{"kind", "side"}, {"text", argument}});
        return Command::kDone;
      case SlashCommandId::kVerbose:
        presenter_.SetDetailed(!presenter_.Detailed());
        WriteTerminalRecord(Note(
            Tone::kNeutral,
            presenter_.Detailed()
                ? "verbose on — full reasoning and tool output"
                : "verbose off — compact reasoning and tool output"));
        wake_.Wake();
        return Command::kDone;
      case SlashCommandId::kShare:
        Send({{"kind", "share"}});
        return Command::kDone;
      default:
        return Command::kPass;
    }
  }
  // Says so when a turn is running, which switching would abandon.
  bool TurnActive(const std::string& before) {
    std::lock_guard lock(mutex_);
    if (waiting_.empty() && !running_) return false;
    WriteTerminalRecord(
        Note(Tone::kNeutral, "turn active; interrupt it before " + before));
    return true;
  }
  // Ends the input loop to open `target` next, unless a turn is running.
  Command Leave(const std::string& target, const std::string& before) {
    if (TurnActive(before)) return Command::kDone;
    std::lock_guard lock(mutex_);
    next_ = target;
    return Command::kLeave;
  }
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
      // A coordinator holding thread events says why, once per change.
      if (std::string paused = JsonValue(state, "paused", "");
          paused != paused_) {
        paused_ = std::move(paused);
        if (!paused_.empty()) {
          presenter_.Consume(
              NoticeEvent(PresentationStatus::kWarned, paused_, false));
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
            block_kind == "user") {
          presenter_.Block(block);
        }
      } else if (type == "command.completed" && data.contains("output")) {
        std::string output = JsonValue(data, "output", "");
        if (output.empty() && data.contains("result") &&
            !data["result"].empty()) {
          output = JsonDump(data["result"], 2);
        }
        // Command replies are this program's own rows, joins and all.
        if (!output.empty()) {
          WriteTerminalRecord(AsciiGlyphs(TerminalSafe(output)) + "\n");
        }
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
      if (result.contains("answer")) {
        WriteTerminalRecord(StyledBlock(AsciiGlyphs("side · not in history"),
                                        DIM()) +
                            TerminalSafe(JsonValue(result, "answer", "")) +
                            "\n");
      } else if (!result.empty()) {
        WriteTerminalRecord(TerminalSafe(JsonDump(result, 2)) + "\n");
      }
      // A coordinator rewinds in place: reopening its path shows the
      // shortened conversation, with the message back in the composer.
      if (JsonValue(result, "forked", false) ||
          JsonValue(result, "rewound", false)) {
        std::lock_guard lock(mutex_);
        next_ = JsonValue(result, "path", "");
        if (rewinding_) carry_ = JsonValue(result, "prompt", "");
        navigate_ = true;
        wake_.Wake();
      }
      rewinding_ = false;
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
    std::string frame;
    Unmount(frame);
    output_.Write(frame);
  }
  // Erases the composer, the status row and the tail's last row, the one row
  // of it still growing. The rows above are complete, so they stay: past the
  // top of the screen no erase could reach them anyway. Returns how many
  // bytes of the tail are still on screen.
  size_t Unmount(std::string& frame) {
    if (!composer_.Drawn()) return 0;
    const size_t width = TerminalWidth();
    frame += "\r" +
             CursorUp(composer_.CaretRow() + 1 + (tail_.empty() ? 0 : 1) +
                      StatusOverflowRows(status_columns_, width)) +
             EraseBelow();
    composer_.Detach();
    return LastRowStart(tail_, width);
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
        interrupting_, presenter_.Detailed());
    const bool resized = g_terminal_resized != 0;
    g_terminal_resized = 0;
    if (!update.changed && !resized && composer_.Drawn()) {
      if (status == last_status_) return;
      const size_t rows = composer_.CaretRow() + 1;
      output_.Write(
          "\r" + CursorUp(rows) + StatusBarLine(status, &status_columns_) +
          "\033[" + std::to_string(rows) + "B\r" +
          (composer_.CaretColumn()
               ? "\033[" + std::to_string(composer_.CaretColumn()) + "C"
               : ""));
    } else {
      // One write, so the terminal never shows the erase without its redraw.
      std::string frame;
      const size_t kept = Unmount(frame);
      // What follows the tail continues it: committed text starts with the
      // tail it finished, and a longer tail with the shorter one.
      std::string text = tail_;
      if (update.changed) {
        text = update.committed + update.tail;
        tail_ = std::move(update.tail);
      }
      frame += ActiveSgr(std::string_view(text).substr(0, kept)) +
               text.substr(kept);
      if (!tail_.empty()) frame += "\n";
      frame += StatusBarLine(status, &status_columns_) + "\n";
      composer_.Remount(frame);
      output_.Write(frame);
    }
    last_status_ = std::move(status);
  }

  Connection connection_;
  std::string path_, decision_, draft_, tail_, waiting_, next_, last_status_;
  std::string next_folder_;
  std::string paused_;
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
  std::atomic<bool> rewinding_{false};
  std::string carry_;
  std::atomic<bool> input_blocked_{true}, interaction_{false};
  std::set<std::string> shown_;
  std::set<std::string> own_requests_, echoed_;
};
}  // namespace
int CoordinatorPromptMain(const Options& options) {
  const std::string cwd = CanonicalCwd();
  const std::string path = CoordinatorPath(cwd);
  std::string error;
  auto connection = Open(ExecutablePath(), cwd, path, "", options, error);
  if (!connection.socket) {
    fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  auto send = [&](json command) {
    command["v"] = kProtocol;
    command["session_id"] = HashHex(path);
    command["generation"] = connection.generation;
    command["request_id"] = command.value("request_id", RandomToken(16));
    return WriteFrame(connection.socket.Get(), command);
  };
  const std::string request = RandomToken(16);
  bool submitted = false, rejected = false, completed = false;
  json stop = json::object();
  // The coordinator's session runs on; this request is its growth.
  Usage before, after;
  Pipe never;
  if (!never.Open()) return 1;
  ReadFrames(connection.socket.Get(), never.read.Get(), kFrameBytes,
             [&](const json& frame) {
               const std::string kind = JsonValue(frame, "kind", "");
               if (kind == "outcome" &&
                   JsonValue(frame, "request_id", "") == request &&
                   !JsonValue(frame, "accepted", false)) {
                 error = JsonValue(frame, "error", "coordinator refused");
                 rejected = true;
                 return false;
               }
               if (kind != "state") return true;
               if (!submitted && !JsonValue(frame, "busy", true)) {
                 before = UsageFromJson(
                     JsonValue(frame["state"], "usage", json::object()));
                 submitted = send({{"kind", "submit"},
                                   {"request_id", request},
                                   {"text", options.prompt}});
                 return submitted;
               }
               // Nobody is here to approve: a question is declined.
               if (const json* pending = JsonObject(frame, "pending")) {
                 fprintf(stderr, "· declined: %s\n",
                         TerminalSafe(JsonValue(*pending, "prompt", "approval"))
                             .c_str());
                 send({{"kind", "reply"},
                       {"interaction_id", JsonValue(*pending, "id", "")},
                       {"text", ""}});
               }
               if (JsonValue(frame, "checkpoint", false) &&
                   JsonValue(frame, "completed_request_id", "") == request) {
                 // A queued thread event may already have started the next
                 // turn, which clears the stop record.
                 stop = JsonValue(frame["state"], "stop", json::object());
                 after = UsageFromJson(
                     JsonValue(frame["state"], "usage", json::object()));
                 completed = true;
                 return false;
               }
               return true;
             });
  if (rejected || !completed) {
    fprintf(stderr, "%s\n",
            error.empty() ? "coordinator runtime closed" : error.c_str());
    return 1;
  }
  SessionLoadResult saved = SessionStore::Inspect(path);
  Conversation conversation;
  std::string answer;
  if (saved.record &&
      std::move(saved.record->state).RestoreConversation(conversation)) {
    answer = conversation.LastAssistantText();
  }
  if (options.json) {
    printf("%s\n", JsonDump({{"answer", answer},
                             {"session_id", HashHex(path)},
                             {"usage", UsageJson(UsageDifference(after, before))},
                             {"stop", stop}})
                       .c_str());
  } else {
    printf("%s\n", answer.c_str());
  }
  const std::string reason = JsonValue(stop, "reason", "completed");
  return reason == "completed" ? 0 : 1;
}

int TerminalMain(Options options) {
  std::string path;
  if (options.Coordinator()) {
    path = CoordinatorPath(CanonicalCwd());
  } else if (options.resume_pick) {
    path = PickSession();
  } else if (options.resume_latest) {
    auto sessions = ListSessions();
    if (!sessions.empty()) path = sessions.front().path;
  }
  std::string draft, folder = CanonicalCwd();
  for (;;) {
    // A saved session reopens in its own folder; so does a coordinator
    // reached from a session in another directory.
    std::string cwd =
        JsonValue(SessionHeader(path), kSessionHeaderCwd, folder);
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
    const bool coordinator = path == CoordinatorPath(cwd);
    if (coordinator) printf("%s", TerminalSafe(CoordinatorBoard(cwd)).c_str());
    Terminal terminal(std::move(connection), path, draft);
    int result = terminal.Run(options.attach_paths);
    draft = terminal.Carry();
    options.attach_paths.clear();
    if (result || terminal.Next().empty()) return result;
    if (terminal.Next() == "/restart") {
      // The runtime was asked to close; once it is gone the next Open
      // starts a fresh one on the same history.
      for (int attempt = 0; attempt < 100 && PathExists(SocketPath(path));
           ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      fputs(Note(Tone::kNeutral, "restarted").c_str(), stdout);
    } else if (terminal.Next() == "/reset") {
      // The folder has one coordinator; its reset keeps the same file.
      if (!coordinator) path.clear();
    } else {
      path = terminal.Next();
      if (!terminal.NextFolder().empty()) folder = terminal.NextFolder();
    }
  }
}
}  // namespace uagent::session
