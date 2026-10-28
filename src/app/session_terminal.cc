// Copyright 2026 Timon Gentzsch
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/launch.h"
#include "include/app/panel.h"
#include "include/app/session.h"
#include "include/cli/cli.h"
#include "include/core/effective_config.h"
#include "include/core/fs.h"
#include "include/core/runtime_config.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/core/verbosity.h"
#include "include/ui/ask_picker.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/sessions.h"
#include "src/app/session_terminal_internal.h"

namespace uagent::session {
namespace {
// Output is drawn at most this often, however fast it arrives.
constexpr auto kPaintInterval = std::chrono::milliseconds(16);
}  // namespace

const DetailPolicy& ConfiguredDetail(const RuntimeConfig::Values& overrides) {
  return DetailFor(ConfigManager::Capture(overrides).Read().Get(
      std::string(kVerbositySetting)));
}

int Terminal::Run(const std::vector<std::string>& attachments) {
  if (!Attach()) return 1;
  std::vector<std::string> files = attachments;
  std::string cooked;
  int exit_code = 0;
  // One paint per wake: at once after a key, otherwise once a frame, so
  // output that arrives faster than that is drawn in batches.
  bool stale = true, typed = true;
  std::chrono::steady_clock::time_point painted;
  while (!disconnected_ && !navigate_) {
    if (raw_ && stale &&
        (typed ||
         std::chrono::steady_clock::now() >= painted + kPaintInterval)) {
      Paint();
      painted = std::chrono::steady_clock::now();
      stale = typed = false;
    }
    bool running;
    bool needs_model;
    {
      std::lock_guard lock(mutex_);
      running = running_;
      // Another terminal's picker, or a command running, is not asked twice.
      needs_model = state_.contains("route") &&
                    JsonValue(state_, "route", "x").empty() &&
                    pending_.is_null() && !running_ && !input_blocked_;
    }
    // A conversation that has no model yet starts at the picker, once.
    if (needs_model && !model_asked_) {
      model_asked_ = true;
      Send({{"kind", "submit"}, {"text", "/model all"}});
    }
    g_streaming = running ? 1 : 0;
    SetQuitGesture(true);
    bool keyed = false;
    if (!Wait(running,
              stale ? std::optional(painted + kPaintInterval) : std::nullopt,
              keyed)) {
      break;
    }
    stale = true;
    if (Interrupted()) {
      exit_code = 130;
      break;
    }
    json pending;
    {
      std::lock_guard lock(mutex_);
      pending = pending_;
      running = running_;
    }
    std::string decision = JsonValue(pending, "id", "");
    if (decision != decision_) {
      decision_ = decision;
      // The output before the decision is shown before it.
      if (raw_) Absorb();
      if (PresentDecision(pending, decision)) continue;
    }
    if (!keyed &&
        !(raw_ && (composer_.HasPending() || composer_.WakeDeadline()))) {
      continue;
    }
    const std::optional<InteractiveInputEvent> input = ReadInput(cooked);
    if (!input) break;
    typed = true;
    if (Handle(*input, pending, decision, running, files) == Command::kLeave) {
      break;
    }
  }
  return Detach(files, exit_code);
}

bool Terminal::Attach() {
  if (!stop_.Open() || !wake_.Open()) return false;
  // Plain mode reads cooked lines: the composer repaints in place.
  raw_ =
      !g_plain && isatty(STDIN_FILENO) && output_.Start() && composer_.Start();
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
      WriteTerminalRecord("Session connection closed; use -c to reconnect.\n");
    }
    disconnected_ = true;
    wake_.Wake();
  });
  if (raw_) {
    region_.Commit("Connecting\n");
    composer_.Mount(InputPrompt(), draft_);
  }
  return true;
}

bool Terminal::Wait(bool running,
                    std::optional<std::chrono::steady_clock::time_point> frame,
                    bool& keyed) {
  pollfd waits[] = {
      {!raw_ && input_blocked_ && !interaction_ ? -1 : STDIN_FILENO, POLLIN, 0},
      // Output waiting for its frame is read when the frame comes.
      {raw_ && !frame ? output_.ReadFd() : -1, POLLIN, 0},
      {wake_.read.Get(), POLLIN, 0},
      {AbortWakeFd(), POLLIN, 0}};
  int timeout = -1;
  if (const auto deadline = composer_.WakeDeadline(); raw_ && deadline) {
    timeout = PollTimeoutMs(*deadline);
  }
  // The working row's spinner and elapsed time advance on their own;
  // without motion only the seconds do.
  if (raw_ && running) {
    const int tick = static_cast<int>(
        std::chrono::milliseconds(g_motion ? kSpinnerPeriod
                                           : std::chrono::seconds(1))
            .count());
    timeout = timeout < 0 ? tick : std::min(timeout, tick);
  }
  if (raw_ && frame) {
    const int next = PollTimeoutMs(*frame);
    timeout = timeout < 0 ? next : std::min(timeout, next);
  }
  if (poll(waits, 4, timeout) < 0 && errno != EINTR) return false;
  keyed = waits[0].revents & (POLLIN | POLLHUP);
  wake_.Drain();
  return true;
}

bool Terminal::Interrupted() {
  if (TakeIdleInterrupt()) {
    if (quit_hint_) return true;
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
  return false;
}

std::optional<InteractiveInputEvent> Terminal::ReadInput(std::string& cooked) {
  if (raw_) return composer_.Read();
  char byte;
  ssize_t count = read(STDIN_FILENO, &byte, 1);
  if (count <= 0) return std::nullopt;
  if (byte != '\n') {
    cooked += byte;
    return InteractiveInputEvent{};
  }
  InteractiveInputEvent input{InteractiveInputKind::kLine, std::move(cooked)};
  cooked.clear();
  return input;
}

Terminal::Command Terminal::Handle(const InteractiveInputEvent& input,
                                   const json& pending,
                                   const std::string& decision, bool running,
                                   std::vector<std::string>& files) {
  if (input.kind == InteractiveInputKind::kNone) return Command::kDone;
  quit_hint_ = false;
  if (input.kind == InteractiveInputKind::kEof) return Command::kLeave;
  if (input.kind == InteractiveInputKind::kEscape) {
    if (raw_) composer_.Clear();
    draft_.clear();
    if (!decision.empty()) {
      Reply(decision, "");
    }
    if (running) {
      Send({{"kind", "interrupt"}});
      interrupting_ = true;
    }
    return Command::kDone;
  }
  if (input.kind == InteractiveInputKind::kBackground) {
    Send({{"kind", "activity"}, {"operation", "background"}});
    return Command::kDone;
  }
  if (input.kind != InteractiveInputKind::kLine) return Command::kDone;
  if (raw_) {
    if (decision.empty() && !Trim(input.text).empty()) {
      // A streaming tail is finished where it stands, above the echo.
      Absorb();
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
  if (handled != Command::kPass) return handled;
  if (!decision.empty()) {
    // Without raw input an ask is answered on one line.
    if (JsonValue(pending, "kind", "") == "ask") {
      text = AskAnswersFromLine(JsonValue(pending, "questions", json::array()),
                                text);
    }
    Reply(decision, text);
  } else if (!text.empty() || !files.empty()) {
    json command = {{"kind", "submit"}, {"text", text}};
    if (!files.empty()) command["attachments"] = files;
    Send(std::move(command));
    files.clear();
  }
  return Command::kDone;
}

int Terminal::Detach(const std::vector<std::string>& files, int exit_code) {
  detaching_ = true;
  if (!files.empty()) {
    // Staged via /attach but never submitted (EOF/quit/compose-cancel):
    // say so instead of dropping them silently.
    const std::string dropped =
        Note(Tone::kNeutral, Plural(files.size(), "staged attachment") +
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
      pollfd ready[] = {{wake_.read.Get(), POLLIN, 0},
                        {raw_ ? output_.ReadFd() : -1, POLLIN, 0}};
      poll(ready, 2, -1);
      wake_.Drain();
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

namespace {
// Once per home, before the first conversation: what to try and the keys
// that are easy to miss.
void Welcome() {
  const std::string marker = UagentDir(kConfigDir) + "/welcomed";
  if (PathExists(marker)) return;
  std::string error;
  if (!WritePrivateFile(marker, "", error)) return;
  fputs(AsciiGlyphs("Welcome to µAgent. Try \"explain this repository\", "
                    "\"fix the failing test\" or /help.\n"
                    "Enter sends · Esc stops a turn · / lists commands · "
                    "/undo puts back the last turn's file changes\n"
                    "uagent panel opens a folder's panel, which runs "
                    "several conversations.\n")
            .c_str(),
        stdout);
}
}  // namespace

int TerminalMain(Options options) {
  Welcome();
  std::string path;
  if (options.Panel()) {
    path = PanelPath(CanonicalCwd());
  } else if (options.resume_pick) {
    path = PickSession();
  } else if (options.resume_latest) {
    auto sessions = ListSessions();
    if (!sessions.empty()) path = sessions.front().path;
  }
  std::string draft, side_question, folder = CanonicalCwd();
  const bool pinned =
      options.overrides.contains(std::string(kVerbositySetting));
  const DetailPolicy* detail = &ConfiguredDetail(options.overrides);
  bool redrawn = false;
  TerminalInputDecoder keys;
  for (;;) {
    // A saved session reopens in its own folder; so does a panel
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
    const bool panel = path == PanelPath(cwd);
    if (panel) {
      const std::string board = PanelBoard(cwd);
      printf("%s", TerminalSafe(board).c_str());
      // Nothing to manage yet: what it is for, as the web's empty chat says.
      if (FolderSessions(cwd).empty()) {
        fputs(AsciiGlyphs("\nAsk for work and it starts threads that do it. "
                          "Ask a question and it fans out\nsubagents and "
                          "reports what held up.\n")
                  .c_str(),
              stdout);
      }
    }
    Terminal terminal(std::move(connection), path, *detail, pinned, redrawn,
                      keys, draft);
    if (!side_question.empty()) terminal.Ask(side_question);
    int result = terminal.Run(options.attach_paths);
    draft = terminal.Carry();
    side_question = terminal.SideQuestion();
    detail = &terminal.Detail();
    // The same session again is a redraw at another level.
    redrawn = terminal.Next() == path;
    options.attach_paths.clear();
    if (result || terminal.Next().empty()) return result;
    if (terminal.Next() == "/restart") {
      // Once the runtime is gone the next Open starts a fresh one on the
      // same history.
      const std::string still = CloseRuntime(path);
      fputs(Note(Tone::kNeutral, still.empty() ? "restarted" : still).c_str(),
            stdout);
    } else if (terminal.Next() == "/reset") {
      // The folder has one panel; its reset keeps the same file.
      if (!panel) path.clear();
    } else {
      if (redrawn) fputs(ClearScreen(), stdout);
      path = terminal.Next();
      if (!terminal.NextFolder().empty()) folder = terminal.NextFolder();
    }
  }
}
}  // namespace uagent::session
