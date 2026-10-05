// Copyright 2026 Timon Gentzsch
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/config_proposal.h"
#include "include/app/coordinator.h"
#include "include/app/launch.h"
#include "include/app/session.h"
#include "include/cli.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/md.h"
#include "include/ui/ask_picker.h"
#include "include/ui/display.h"
#include "include/ui/editor.h"
#include "include/ui/interactive.h"
#include "include/ui/live_region.h"
#include "include/ui/presentation.h"
#include "include/ui/sessions.h"

namespace uagent::session {
namespace {
// The pinned row from the worker's state frame: the working row while a turn
// runs, the session row otherwise -- the same renderers presentation_test pins.
std::string StatusRow(const json& state,
                      std::chrono::steady_clock::duration elapsed,
                      bool interrupting, std::string_view verbosity) {
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
                    .verbosity = std::string(verbosity),
                    .background = background + subagents});
}

// The level this terminal shows: its --verbosity when given, else the
// configured one.
const DetailPolicy& ConfiguredDetail(const RuntimeConfig::Values& overrides) {
  const auto values = ConfigManager::Capture(false, overrides).Read().values;
  const auto found = values.find(std::string(kVerbositySetting));
  return DetailFor(found == values.end() ? "" : found->second);
}

class Terminal {
 public:
  // `draft` starts the composer, e.g. the message a rewind forked before.
  // `pinned`: --verbosity set this terminal's level, so it follows no other.
  // `redrawn`: the conversation is shown again because the level changed.
  Terminal(Connection connection, std::string path, const DetailPolicy& detail,
           bool pinned, bool redrawn, std::string draft = "")
      : connection_(std::move(connection)),
        path_(std::move(path)),
        draft_(std::move(draft)),
        region_(output_),
        composer_(output_, region_),
        detail_(&detail),
        pinned_(pinned),
        redrawn_(redrawn) {
    presenter_.SetDetail(detail);
  }
  const DetailPolicy& Detail() const { return *detail_; }

  // Shows a decision that just arrived, or gives the composer back once it
  // is gone. True when it was settled here and the loop starts over.
  bool PresentDecision(const json& pending, const std::string& decision) {
    if (!decision.empty() && JsonValue(pending, "kind", "") == "ask" && raw_) {
      const json answered =
          PickAskAnswers(JsonValue(pending, "questions", json::array()),
                         region_, [this, decision] {
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
      return true;
    }
    if (!decision.empty() && JsonValue(pending, "kind", "") == "editor") {
      std::string text = JsonValue(pending, "initial", "");
      bool edited =
          raw_ ? composer_.EditTextExternally(text)
               : EditExternalText(text, STDIN_FILENO, kAdaptiveSystemBytes);
      Send({{"kind", "reply"},
            {"interaction_id", decision},
            {"text", text},
            {"cancelled", !edited}});
      return true;
    }
    if (!decision.empty()) {
      std::string description;
      if (const json* approval = JsonObject(pending, "approval")) {
        std::string risks;
        for (const json& risk : JsonValue(*approval, "risks", json::array())) {
          risks += (risks.empty() ? "Risk: " : " · ") +
                   JsonValue(risk, "label", std::string());
        }
        if (!risks.empty()) description = AsciiGlyphs(risks) + "\n";
        description += JsonValue(*approval, "mandatory_reason", "") + "\n" +
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
        region_.Commit(ColorizeDiffLines(TerminalSafe(description)));
      } else {
        fputs(TerminalSafe(description).c_str(), stdout);
      }
    }
    const std::string prompt = TerminalSafe(
        DecisionPrompt(JsonValue(pending, "prompt", ""),
                       JsonValue(pending, "options", json::array())));
    if (raw_) {
      if (!decision.empty()) {
        draft_ = composer_.Buffer();
        region_.Commit(prompt);
        composer_.Mount(InputPrompt(), JsonValue(pending, "initial", ""),
                        false);
      } else {
        composer_.Mount(InputPrompt(), draft_);
      }
    } else if (!decision.empty()) {
      const char* label = !g_plain ? ""
                          : JsonValue(pending, "kind", "") == "ask"
                              ? "question: "
                              : "approval needed: ";
      printf("%s%s\n", label, prompt.c_str());
    }
    return false;
  }

  int Run(const std::vector<std::string>& attachments) {
    if (!stop_.Open() || !wake_.Open()) return 1;
    // Plain mode reads cooked lines: the composer repaints in place.
    raw_ = !g_plain && isatty(STDIN_FILENO) && output_.Start() &&
           composer_.Start();
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
      region_.Commit("Connecting\n");
      composer_.Mount(InputPrompt(), draft_);
    }
    std::vector<std::string> files = attachments;
    std::string cooked;
    bool quit = false;
    int exit_code = 0;
    while (!quit && !disconnected_ && !navigate_) {
      if (raw_) Paint();  // the last input's draft
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
          composer_.Clear();
          region_.Commit("Press Ctrl+C again to detach.\n");
        } else {
          output_.Write("Press Ctrl+C again to detach.\n");
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
        if (PresentDecision(pending, decision)) continue;
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
        if (decision.empty() && !Trim(input.text).empty()) {
          // A streaming tail is finished where it stands, above the echo.
          region_.Commit((tail_.empty() ? "" : tail_ + "\n") +
                         UserEchoRow(InputPrompt(), TerminalSafe(input.text)));
          if (!tail_.empty()) {
            output_.AdoptTail();
            tail_.clear();
            region_.SetTail("");
          }
        }
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
      const std::string dropped = Note(
          Tone::kNeutral, std::to_string(files.size()) + " staged attachment" +
                              (files.size() == 1 ? "" : "s") +
                              " discarded: nothing was submitted");
      if (raw_) {
        region_.Commit(dropped);
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
      region_.Clear();
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
    std::string folder = JsonValue(
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
          region_.Clear();
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
        std::string target =
            argument.empty() ? PickSession() : MatchSessionPrefix(argument);
        if (!argument.empty() && target.empty()) {
          WriteTerminalRecord(Note(
              Tone::kNeutral,
              "no unique session matches \"" + TerminalSafe(argument) + "\""));
        }
        if (target.empty()) return Command::kDone;
        return Leave(target, "switching sessions");
      }
      default:
        break;
    }
    if (deciding) return Command::kPass;
    switch (slash.spec->id) {
      case SlashCommandId::kAttach: {
        // Bare /attach lists them (host). Terminals quote dropped paths
        // containing spaces; strip one surrounding pair so a drop Just Works.
        if (argument.empty()) return Command::kPass;
        const std::string file = Unquote(argument);
        if (file == "clear") {
          files.clear();
        } else {
          files.push_back(CanonicalAccessPath(file).string());
        }
        return Command::kDone;
      }
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
      case SlashCommandId::kVerbosity: {
        if (argument.empty()) {
          WriteTerminalRecord(
              Note(Tone::kNeutral,
                   "verbosity " + std::string(Detail().level) +
                       (pinned_ ? " · this terminal only (--verbosity)" : "")));
          return Command::kDone;
        }
        if (std::ranges::find(kVerbosityLevels, argument) ==
            std::end(kVerbosityLevels)) {
          WriteTerminalRecord(
              Note(Tone::kNeutral, "usage: /verbosity minimal|default|full"));
          return Command::kDone;
        }
        // The level is one setting for every terminal and browser; a pinned
        // terminal changes only itself.
        if (!pinned_) {
          const json saved = ConfigurationControl(
              {{"operation", "apply"},
               {"changes", json::array({{{"key", kVerbositySetting},
                                         {"value", argument}}})}},
              ConfigManager::Capture(false, {}));
          if (const std::string error = JsonValue(saved, "error", "");
              !error.empty()) {
            WriteTerminalRecord(Note(Tone::kError, TerminalSafe(error)));
            return Command::kDone;
          }
        }
        Restyle(DetailFor(argument));
        return Command::kDone;
      }
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
    StampFrame(command, HashHex(path_), connection_.generation);
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
    static const json kNone = json::object();
    auto kind = JsonValue(frame, "kind", "");
    if (kind == "state") {
      auto sent = frame.find("state");
      const json& state = sent != frame.end() ? *sent : kNone;
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
        const json* view = JsonObject(state, "view");
        if (const json* blocks = view ? JsonArray(*view, "blocks") : nullptr) {
          for (const auto& block : *blocks) {
            shown_.insert(JsonValue(block, "id", ""));
            presenter_.Block(block);
          }
          history_ = true;
          // Why the screen changed, where the eye is after a redraw.
          if (std::exchange(redrawn_, false)) {
            WriteTerminalRecord(Note(
                Tone::kNeutral, "verbosity " + std::string(Detail().level)));
          }
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
      auto sent = frame.find("data");
      const json& data = sent != frame.end() ? *sent : kNone;
      if (type == "session.ended") ended_ = true;
      if (type == "notice" && !history_) return;
      if (type == "usage.updated") {
        std::lock_guard lock(mutex_);
        ApplySessionEvent(state_, type, data);
        wake_.Wake();
      } else if (type == "message.changed") {
        const json* found = JsonObject(data, "block");
        const json& block = found ? *found : kNone;
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
        // The level changed elsewhere (another terminal, the web, the file).
        if (const json* changed = JsonArray(data, "changed");
            type == "config.changed" && !pinned_ && changed &&
            std::ranges::find(*changed, json(kVerbositySetting)) !=
                changed->end()) {
          if (const DetailPolicy& detail = ConfiguredDetail({});
              &detail != detail_) {
            Restyle(detail);
          }
        }
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
        WriteTerminalRecord(
            StyledBlock(AsciiGlyphs("side · not in history"), DIM()) +
            TerminalSafe(JsonValue(result, "answer", "")) + "\n");
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
  // Shows the conversation at another level. A terminal cannot restyle rows
  // it printed, so it attaches again and the conversation is replayed; plain
  // and piped output is not repeated, only what follows changes.
  void Restyle(const DetailPolicy& detail) {
    detail_ = &detail;
    if (raw_) {
      next_ = path_;
      navigate_ = true;
    } else {
      presenter_.SetDetail(detail);
      WriteTerminalRecord(
          Note(Tone::kNeutral, "verbosity " + std::string(detail.level)));
    }
    wake_.Wake();
  }
  // Brings the live region up to date: new output, the status row and the
  // draft, repainted only where they changed.
  void Paint() {
    auto update = output_.Read();
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
    if (update.changed) {
      region_.Commit(std::move(update.committed));
      tail_ = std::move(update.tail);
      region_.SetTail(tail_);
    }
    region_.SetStatus(StatusBarLine(StatusRow(
        state,
        turn_started_ ? std::chrono::steady_clock::now() - *turn_started_
                      : std::chrono::steady_clock::duration{},
        interrupting_, Detail().level)));
    const RawComposer::Layout draft = composer_.View();
    region_.SetComposer(draft.rows, draft.caret_row, draft.caret_col);
    region_.Flush();
  }

  Connection connection_;
  std::string path_, decision_, draft_, tail_, waiting_, next_;
  std::string next_folder_;
  std::string paused_;
  InteractiveOutput output_;
  LiveRegion region_;
  RawComposer composer_;
  TerminalPresenter presenter_;
  Pipe stop_, wake_;
  std::thread reader_;
  std::mutex mutex_, send_mutex_;
  json state_ = json::object(), pending_;
  bool running_ = false, history_ = false, raw_ = false;
  bool quit_hint_ = false;
  std::optional<std::chrono::steady_clock::time_point> turn_started_;
  std::atomic<bool> interrupting_{false};
  std::atomic<bool> disconnected_{false}, detaching_{false}, ended_{false},
      failed_{false};
  std::atomic<bool> navigate_{false};
  std::atomic<bool> rewinding_{false};
  std::string carry_;
  std::atomic<const DetailPolicy*> detail_;
  const bool pinned_;
  bool redrawn_;
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
    StampFrame(command, HashHex(path), connection.generation);
    command["request_id"] = command.value("request_id", RandomToken(16));
    return WriteFrame(connection.socket.Get(), command);
  };
  const std::string request = RandomToken(16);
  bool submitted = false, rejected = false, completed = false, busy = true;
  bool paused = false, done = false;
  std::string probe;  // the request that asks the coordinator how it stands
  json stop = json::object();
  // The coordinator's session runs on; this request is its growth.
  Usage before, after;
  const auto receive = [&](const json& frame) {
    const std::string kind = JsonValue(frame, "kind", "");
    if (kind == "outcome" && JsonValue(frame, "request_id", "") == request &&
        !JsonValue(frame, "accepted", false)) {
      error = JsonValue(frame, "error", "coordinator refused");
      rejected = true;
      return false;
    }
    // Its state came just before, and nothing is owed: it stands as it said.
    if (kind == "outcome" && !probe.empty() &&
        JsonValue(frame, "request_id", "") == probe) {
      probe.clear();
      done = !busy;
      return !done;
    }
    if (kind != "state") return true;
    if (!submitted && !JsonValue(frame, "busy", true)) {
      before =
          UsageFromJson(JsonValue(frame["state"], "usage", json::object()));
      submitted = send({{"kind", "submit"},
                        {"request_id", request},
                        {"text", options.prompt}});
      return submitted;
    }
    // Nobody is here to approve: a question is declined.
    if (const json* pending = JsonObject(frame, "pending")) {
      fprintf(stderr, "· declined: %s\n",
              TerminalSafe(JsonValue(*pending, "prompt", "approval")).c_str());
      send({{"kind", "reply"},
            {"interaction_id", JsonValue(*pending, "id", "")},
            {"text", ""}});
    }
    // A report it has taken waits as guidance until its turn starts.
    busy = JsonValue(frame, "busy", false) || JsonValue(frame, "guidance", 0);
    paused = frame["state"].contains("paused");
    if (!JsonValue(frame, "checkpoint", false) ||
        (!completed &&
         JsonValue(frame, "completed_request_id", "") != request)) {
      return true;
    }
    // A queued thread event may already have started the next turn, which
    // clears the stop record: the request's own is kept.
    if (!completed) stop = JsonValue(frame["state"], "stop", json::object());
    after = UsageFromJson(JsonValue(frame["state"], "usage", json::object()));
    completed = true;
    return true;
  };
  // The work it delegated is part of the answer. Each step is in place
  // before the one before it ends: a thread mails its report before it shows
  // idle, the coordinator queues a report before acknowledging it, and the
  // queue starts its turn. So when no thread owes anything, the coordinator
  // is asked, and its word that it is idle is the end. A thread that waits
  // for a person, or whose runtime is gone, is not waited for, nor are
  // reports the spend limit holds. One buffer for the whole wait: a frame
  // may arrive in pieces on either side of a tick.
  constexpr int kTickMs = 250;
  FrameBuffer frames;
  char buffer[4096];
  for (bool open = true; open && !rejected && !done;) {
    pollfd wait{connection.socket.Get(), POLLIN, 0};
    const int ready = poll(&wait, 1, kTickMs);
    if (ready < 0 && errno != EINTR) break;
    if (ready > 0) {
      const ssize_t count = read(wait.fd, buffer, sizeof buffer);
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      open = count > 0 &&
             frames.Feed(std::string_view(buffer, static_cast<size_t>(count)),
                         receive);
      continue;
    }
    if (!submitted || !completed || busy || !probe.empty() ||
        (!paused && ThreadsOwe(cwd))) {
      continue;
    }
    probe = RandomToken(16);
    open = send({{"kind", "refresh"}, {"request_id", probe}});
  }
  if (rejected || !completed) {
    fprintf(stderr, "%s\n",
            error.empty() ? "coordinator runtime closed" : error.c_str());
    return 1;
  }
  // Everything the coordinator said since the request, in full: a thread's
  // report reopens its turn or starts another, and each says part of it.
  std::string answer;
  SessionLoadResult saved = SessionStore::Inspect(path);
  Conversation conversation;
  if (saved.record &&
      std::move(saved.record->state).RestoreConversation(conversation)) {
    const json& messages = conversation.Messages();
    size_t asked = messages.size();
    for (size_t index = messages.size(); index > 0 && asked == messages.size();
         --index) {
      const json& message = messages[index - 1];
      if (JsonValue(message, "role", "") == "user" &&
          JsonValue(message, "content", "") == options.prompt) {
        asked = index - 1;
      }
    }
    for (size_t index = asked + 1; index < messages.size(); ++index) {
      const std::string text =
          JsonValue(messages[index], "role", "") == "assistant"
              ? JsonValue(messages[index], "content", "")
              : std::string();
      if (!text.empty()) answer += (answer.empty() ? "" : "\n\n") + text;
    }
    // The request itself was compacted away: what was said last.
    if (answer.empty()) answer = conversation.LastAssistantText();
  }
  if (options.json) {
    printf("%s\n",
           JsonDump({{"answer", answer},
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

namespace {
// Once per home, before the first conversation: what to try and the keys
// that are easy to miss.
void Welcome() {
  const std::string marker = UagentDir(kConfigDir) + "/welcomed";
  if (PathExists(marker)) return;
  std::string error;
  if (!AtomicWriteFile(marker, "", kPrivateFileMode, false, error)) return;
  fputs(AsciiGlyphs("Welcome to µAgent. Try \"explain this repository\", "
                    "\"fix the failing test\" or /help.\n"
                    "Enter sends · Esc stops a turn · / lists commands · "
                    "/undo puts back the last turn's file changes\n")
            .c_str(),
        stdout);
}
}  // namespace

int TerminalMain(Options options) {
  Welcome();
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
  const bool pinned =
      options.overrides.contains(std::string(kVerbositySetting));
  const DetailPolicy* detail = &ConfiguredDetail(options.overrides);
  bool redrawn = false;
  for (;;) {
    // A saved session reopens in its own folder; so does a coordinator
    // reached from a session in another directory.
    std::string cwd = JsonValue(SessionHeader(path), kSessionHeaderCwd, folder);
    if (path.empty()) {
      path = HistoryPath(cwd, MakeSessionId());
    }
    std::string error;
    auto connection = Open(ExecutablePath(), cwd, path, "", options, error);
    if (!connection.socket) {
      fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    const bool coordinator = path == CoordinatorPath(cwd);
    if (coordinator) printf("%s", TerminalSafe(CoordinatorBoard(cwd)).c_str());
    Terminal terminal(std::move(connection), path, *detail, pinned, redrawn,
                      draft);
    int result = terminal.Run(options.attach_paths);
    draft = terminal.Carry();
    detail = &terminal.Detail();
    // The same session again is a redraw at another level.
    redrawn = terminal.Next() == path;
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
      if (redrawn) fputs(ClearScreen(), stdout);
      path = terminal.Next();
      if (!terminal.NextFolder().empty()) folder = terminal.NextFolder();
    }
  }
}
}  // namespace uagent::session
