// Copyright 2026 Timon Gentzsch

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/bootstrap.h"
#include "include/app/commands.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"
#include "include/mcp/rpc.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/ui/display.h"
#include "include/ui/interactive.h"
#include "include/ui/sessions.h"

namespace uagent {
namespace {

class Application {
 public:
  explicit Application(AppContext& context)
      : context_(context),
        runtime_(context.runtime),
        api_(runtime_.api),
        agent_(*context.agent),
        saved_revision_(agent_.Revision()) {}

  AppSession Session() {
    return AppSession{context_, attachments_, session_file_, saved_revision_};
  }

  int Run() {
    int attachment_status = LoadInitialAttachments();
    if (attachment_status != 0) return attachment_status;
    if (!context_.options.prompt.empty() && context_.options.resume_latest) {
      ResumeAtStartup();
    }
    return context_.options.prompt.empty() ? RunInteractive() : RunHeadless();
  }

 private:
  int LoadInitialAttachments() {
    for (const std::string& path : context_.options.attach_paths) {
      Attachment attachment;
      std::string error;
      if (!InspectAttachment(path, attachment, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
      }
      attachments_.push_back(std::move(attachment));
    }
    return 0;
  }

  void ReloadConfigAtTurnBoundary() {
    std::optional<ConfigReload> reload =
        context_.config_manager.Reload(runtime_.config);
    if (!reload) return;
    runtime_.config = reload->active;
    api_.config = reload->active;
    Emit(Event{EventId::kConfigChanged,
               {{"changed", reload->applied},
                {"deferred", reload->deferred},
                {"source", "config_file"}}});
    if (context_.options.prompt.empty() &&
        (!reload->applied.empty() || !reload->deferred.empty())) {
      printf("%s· configuration reloaded for the next turn", DIM());
      if (!reload->deferred.empty()) {
        if (reload->deferred.size() == 1) {
          printf(" · 1 setting requires restart");
        } else {
          printf(" · %zu settings require restart", reload->deferred.size());
        }
      }
      printf("%s\n", RST());
      fflush(stdout);
    }
  }

  void RunTurns(std::string input, json content = nullptr) {
    ReloadConfigAtTurnBoundary();
    agent_.Turn(input, std::move(content));
    SteeringState().Take();
  }

  void LogSessionEnd(const char* reason) const {
    Emit(Event{EventId::kSessionEnded,
               {{"reason", reason},
                {"usage", UsageJson(agent_.SessionUsage())},
                {"context_tokens", agent_.ContextUsed()}}});
  }

  // Release owned processes and per-session files, then drain observations.
  void Teardown(const char* reason) {
    runtime_.Shutdown();
    LogSessionEnd(reason);
    std::remove(UsageLedger().c_str());
    if (!session_file_.empty()) {
      std::string error;
      if (!context_.observability.Journal().Flush(
              session_file_ + ".events.jsonl", error)) {
        fprintf(stderr, "cannot save session journal: %s\n", error.c_str());
      }
    }
    context_.observability.Flush();
  }

  int FinishHeadless(std::string answer, std::string error, int exit_code) {
    Teardown(exit_code == 0 ? "headless_complete" : "headless_error");
    if (context_.options.json_stream || context_.options.json) {
      json envelope = HeadlessResult(
          std::move(answer), std::move(error), agent_.LatestToolTrace(),
          agent_.SessionUsage(), agent_.RouteUsageJson(), exit_code);
      if (context_.options.json_stream) {
        Emit(Event{exit_code == 0 ? EventId::kAnswer : EventId::kError,
                   std::move(envelope)});
      } else {
        printf("%s\n", JsonDump(envelope).c_str());
      }
    } else if (exit_code == 0) {
      printf("%s\n", TerminalSafe(answer).c_str());
    } else {
      fprintf(stderr, "%s\n", TerminalSafe(error).c_str());
    }
    return exit_code;
  }

  int RunHeadless() {
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(context_.options.prompt, attachments_, error,
                                  api_.capabilities.image_input,
                                  !api_.config.image_model.empty(),
                                  api_.capabilities.file_input);
      if (!error.empty()) {
        context_.output.Restore();
        return FinishHeadless("", std::move(error), 2);
      }
    }
    RunTurns(context_.options.prompt, std::move(content));
    // Background work is observational and never starts a model turn. Keep the
    // process alive long enough to publish completion and drain retained state.
    while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
      uint64_t generation = runtime_.processes.Generation();
      if (!agent_.DrainBackground()) {
        runtime_.processes.WaitForChange(generation);
      }
    }
    context_.output.Restore();

    std::string ledger = EnvStr("UAGENT_USAGE_FILE");
    if (!ledger.empty()) {
      std::string error;
      json entry = {{"route", agent_.ActiveRoute()},
                    {"routes", agent_.RouteUsageJson()},
                    {"usage", UsageJson(agent_.SessionUsage())}};
      if (!AppendPrivateLine(ledger, JsonDump(entry), error)) {
        fprintf(stderr, "cannot write usage ledger: %s\n", error.c_str());
      }
    }
    std::string answer = agent_.LastText();
    if (!agent_.LastError().empty()) {
      return FinishHeadless("", agent_.LastError(), 1);
    }
    if (answer.empty()) {
      std::string error = agent_.LastError().empty()
                              ? "agent produced no answer"
                              : agent_.LastError();
      return FinishHeadless("", std::move(error), 1);
    }
    return FinishHeadless(std::move(answer), "", 0);
  }

  void ResumeAtStartup() {
    std::string previous_path = session_file_;
    if (context_.options.resume_pick) {
      ResumeInto(agent_, PickSession(), session_file_);
    } else if (context_.options.resume_latest) {
      std::vector<SessionInfo> sessions = ListSessions();
      if (sessions.empty()) {
        printf("%s· no saved sessions%s\n", DIM(), RST());
        fflush(stdout);
      } else {
        ResumeInto(agent_, sessions.front().path, session_file_);
      }
    }
    AppSession session = Session();
    LoadSessionJournal(session, previous_path);
    saved_revision_ = agent_.Revision();
  }

  void SaveSession() {
    if (!persist_ || agent_.MessageCount() <= 1 ||
        agent_.Revision() == saved_revision_) {
      return;
    }
    if (session_file_.empty()) {
      session_file_ =
          UagentDir(kHistoryDir) + "/" + WorkspaceId(CanonicalCwd()) + "/" +
          UtcStamp("%Y%m%dT%H%M%SZ") + "-" + std::to_string(getpid()) + ".json";
    }
    std::error_code error_code;
    std::filesystem::path directory =
        std::filesystem::path(session_file_).parent_path();
    std::filesystem::create_directories(directory, error_code);
    chmod(directory.c_str(), kPrivateDirMode);
    std::string error;
    if (!agent_.Save(session_file_, error)) {
      fprintf(stderr, "cannot save session: %s\n", error.c_str());
      return;
    }
    if (!context_.observability.Journal().Flush(session_file_ + ".events.jsonl",
                                                error)) {
      fprintf(stderr, "cannot save session journal: %s\n", error.c_str());
      return;
    }
    saved_revision_ = agent_.Revision();
  }

  void RunPrompt(std::string input) {
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(
          input, attachments_, error, api_.capabilities.image_input,
          !api_.config.image_model.empty(), api_.capabilities.file_input);
      if (!error.empty()) {
        printf("%s%s%s\n", RED(), error.c_str(), RST());
        fflush(stdout);
        return;
      }
      attachments_.clear();
    }
    RunTurns(std::move(input), std::move(content));
  }

  bool ProcessInput(std::string input) {
    input = Trim(input);
    if (input.empty()) return false;
    if (input[0] == '/') DebugLog("command", {{"command", input}});
    ParsedSlashCommand command = ParseSlashCommand(input);
    if (command.spec) {
      AppSession session = Session();
      if (!RunSlashCommand(session, command)) return false;
      exit_reason_ = "command";
      return true;
    }
    if (input[0] == '/') {
      printf("%s· unknown command %s; use /help%s\n", RED(),
             TerminalSafe(input).c_str(), RST());
      fflush(stdout);
      return false;
    }
    RunPrompt(std::move(input));
    return false;
  }

  // The pinned-region state machine of the persistent composer: every
  // responsibility below used to be a capturing lambda inside Run().
  struct InteractiveLoop {
    explicit InteractiveLoop(Application& app) : app(app), composer(output) {}

    std::string Status() {
      if (!working) {
        return StatusBar(app.api_, app.agent_.SessionUsage(),
                         SessionStatusView(app.Session()));
      }
      ActivityView view;
      view.elapsed = std::chrono::steady_clock::now() - started;
      view.context_used = app.agent_.ContextSnapshot();
      view.context_window = app.api_.ctx_window;
      view.background = app.runtime_.processes.Count();
      view.foreground = app.runtime_.processes.ForegroundCount();
      view.queued = SteeringState().QueuedCount();
      view.interrupting = interrupting;
      return ActivityBar(view);
    }

    std::string RenderedStatus() {
      return StatusBarLine(Status(), &status_columns);
    }

    std::string StatusState() {
      return std::string(interrupting ? "interrupting|" : "working|") +
             CurrentTerminalActivity() + "|" +
             std::to_string(SteeringState().QueuedCount()) + "|" +
             std::to_string(app.runtime_.processes.Count()) + "|" +
             std::to_string(app.runtime_.processes.ForegroundCount());
    }

    // Erase from the top of the pinned region, which after a narrowing resize
    // starts above the status row: a terminal that rewraps has turned the row
    // written at the old width into several, and erasing from the last one
    // would leave the rest on screen for every later repaint to add to.
    void Unmount() {
      if (!composer.Drawn()) return;
      size_t rows_up = composer.CaretRow() + 1 +
                       StatusOverflowRows(status_columns, TerminalWidth());
      output.Write("\r\033[" + std::to_string(rows_up) + "A\033[J");
      composer.Detach();
    }

    // The one place the pinned region is painted: erase what is there, emit any
    // transcript text above it, then the status row and the composer. Every
    // caller differs only in the text it contributes and whether the composer
    // keeps its buffer, so geometry can only be wrong here.
    void Paint(std::string text, const std::string* prompt,
               const std::string& initial, bool keep_history) {
      Unmount();
      if (!text.empty()) {
        if (text.back() != '\n') text += '\n';
        output.Write(text);
      }
      output.Write(RenderedStatus() + "\n");
      if (prompt) {
        composer.Mount(*prompt, initial, keep_history);
      } else {
        composer.Remount();
      }
    }

    void Mount(const std::string& prompt = InputPrompt(),
               const std::string& initial = std::string(),
               bool keep_history = true) {
      Paint({}, &prompt, initial, keep_history);
    }

    void RefreshStatus() {
      if (!composer.Drawn()) return;
      // Stay silent while a drag is still in flight. The width is read per
      // write, so a status row formatted for one width can land in a terminal
      // that has already become narrower — it wraps, the cursor is no longer
      // where the caller believes, and the next write starts mid-line. The
      // settle repaint restores the row once the size stops moving.
      if (resize_settled) return;
      // The status is always the one row immediately above the mounted
      // composer; update it without repainting the editor's footprint.
      size_t rows_up = composer.CaretRow() + 1;
      output.Write("\r\033[" + std::to_string(rows_up) + "A");
      output.Write(RenderedStatus());
      output.Write("\033[" + std::to_string(rows_up) + "B\r");
      if (composer.CaretColumn() > 0) {
        output.Write("\033[" + std::to_string(composer.CaretColumn()) + "C");
      }
    }

    void FlushOutput(bool all) {
      pending_output += output.Read();
      size_t split = all ? pending_output.size() : pending_output.rfind('\n');
      if (split == std::string::npos || split == 0) return;
      if (!all) ++split;
      std::string ready = pending_output.substr(0, split);
      pending_output.erase(0, split);
      Paint(std::move(ready), nullptr, {}, true);
    }

    void StartWork(std::string input) {
      app.agent_.ContextUsed();
      working = true;
      worker_quit = false;
      interrupting = false;
      started = std::chrono::steady_clock::now();
      worker = std::thread([this, input = std::move(input)]() mutable {
        worker_quit = app.ProcessInput(std::move(input));
        working = false;
        broker.Notify();
      });
    }

    Application& app;
    InteractiveOutput output;
    RawComposer composer;
    InputBroker broker;
    std::thread worker;
    std::atomic<bool> working{false};
    std::atomic<bool> worker_quit{false};
    bool interrupting = false;
    bool exit_when_idle = false;
    bool answering = false;
    std::optional<std::string> next_input;
    std::string saved_draft;
    std::string pending_output;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> resize_settled;
    // The width the pinned status row occupies on screen. iTerm2 and every
    // other terminal that rewraps on resize turns a row written at a wider
    // terminal into several physical rows, and the erase in Unmount() has to
    // walk over all of them.
    size_t status_columns = 0;

    void HandleInputEvent(InteractiveInputEvent event) {
      if (event.kind == InteractiveInputKind::kLine && !answering) {
        // Submission has already printed the prompt below the status row.
        // Replace both regions so the transient working row does not enter
        // scrollback above steering or ordinary user input.
        size_t rows_up = composer.LastSubmittedRows() + 1;
        output.Write("\r\033[" + std::to_string(rows_up) + "A\033[J");
        output.Write(UserEchoRow(composer.Prompt(), TerminalSafe(event.text)) +
                     "\n");
      }
      if (event.kind == InteractiveInputKind::kBackground) {
        if (!working || !app.runtime_.processes.RequestForegroundBackground()) {
          output.Write("\a");
        }
        RefreshStatus();
      } else if (answering) {
        bool eof = event.kind == InteractiveInputKind::kEscape ||
                   event.kind == InteractiveInputKind::kEof;
        broker.Answer(std::move(event.text), eof);
        answering = false;
        Mount(InputPrompt(), saved_draft);
      } else if (event.kind == InteractiveInputKind::kEscape) {
        // A bare Escape clears the current input line (and any stashed draft).
        // Still honour a steering/interrupt request if the agent is working, so
        // Esc doubles as the interrupt key.
        saved_draft.clear();
        composer.Clear();
        if (working && SteeringEnabled() && !interrupting) {
          interrupting = true;
          SteeringState().Request();
          app.runtime_.processes.Wake();
          RefreshStatus();
        }
      } else if (event.kind == InteractiveInputKind::kEof) {
        exit_when_idle = true;
        Mount();
      } else {
        std::string input = Trim(event.text);
        if (!input.empty()) {
          if ((input == "/q" || input == "/quit") && working) {
            exit_when_idle = true;
          } else if (working) {
            // Publish guidance before waking passive tool waits. Their
            // generation predicate makes this pairing lost-wakeup-safe.
            SteeringState().Queue(std::move(input));
            app.runtime_.processes.Wake();
          } else if (worker.joinable()) {
            next_input = std::move(input);
          } else {
            StartWork(std::move(input));
          }
        }
        Mount();
      }
    }

    int Run() {
      if (!output.Start()) return -1;
      if (!composer.Start()) return -1;
      SetTerminalWakeFd(broker.NotifyFd());
      app.runtime_.processes.SetNotifyFd(broker.NotifyFd());
      SetInteractiveReadHandler([this](const std::string& prompt, bool* eof,
                                       bool keep_history,
                                       const std::string& initial) {
        return broker.Read(prompt, eof, keep_history, initial);
      });
      SetPersistentComposer(true);

      constexpr auto kResizeSettle = std::chrono::milliseconds(80);

      Mount();
      auto last_redraw = std::chrono::steady_clock::now();
      std::string last_state = StatusState();
      std::vector<pollfd> events;
      events.reserve(3 + app.runtime_.mcp.Servers().size());
      while (!exit_when_idle || working) {
        events = {{STDIN_FILENO, POLLIN, 0},
                  {output.Fd(), POLLIN, 0},
                  {broker.Fd(), POLLIN, 0}};
        if (!working) {
          for (const auto& server : app.runtime_.mcp.Servers()) {
            if (server->alive && server->out >= 0) {
              events.push_back(
                  {server->out,
                   static_cast<int16_t>(POLLIN | POLLHUP | POLLERR), 0});
            }
          }
        }

        std::optional<std::chrono::steady_clock::time_point> wake_deadline =
            composer.WakeDeadline();
        if (working && !answering) {
          auto status_deadline = last_redraw + std::chrono::milliseconds(100);
          wake_deadline = wake_deadline
                              ? std::min(*wake_deadline, status_deadline)
                              : status_deadline;
        }
        if (resize_settled) {
          wake_deadline = wake_deadline
                              ? std::min(*wake_deadline, *resize_settled)
                              : *resize_settled;
        }
        int timeout_ms = -1;
        if (wake_deadline) {
          auto now = std::chrono::steady_clock::now();
          if (*wake_deadline <= now) {
            timeout_ms = 0;
          } else {
            timeout_ms = PollTimeoutMs(*wake_deadline);
          }
        }

        int ready = poll(events.data(), events.size(), timeout_ms);
        if (ready < 0 && errno != EINTR) break;
        if (g_terminal_resized) {
          g_terminal_resized = 0;
          // Dragging an edge emits a burst of SIGWINCH. The flag already
          // coalesces everything that arrives before this wake; the settle
          // window collapses the rest into one repaint instead of flickering
          // through every intermediate width.
          resize_settled = std::chrono::steady_clock::now() + kResizeSettle;
        }
        if (resize_settled &&
            std::chrono::steady_clock::now() >= *resize_settled) {
          resize_settled.reset();
          // A resize must *replace* the pinned region, not add to it: erasing
          // only downward leaves the status row that sits above the cursor, so
          // every repaint would append another one. Walk up to the status row
          // first, the way every other paint does.
          //
          // The status row's own rewrap is accounted for by
          // StatusOverflowRows(). CaretRow() covers the rest exactly when the
          // composer did not reflow — an empty or short draft, or any widening.
          // Narrowing with a draft long enough to soft-wrap can still leave one
          // stale fragment above; the next mount clears it. Pinning that down
          // needs a cursor position report, which is deliberately not in this
          // change.
          if (composer.Drawn()) Paint({}, nullptr, {}, true);
          last_redraw = std::chrono::steady_clock::now();
          last_state = StatusState();
        }

        // Idle MCP stdout participates in the same poll set; any event also
        // drains messages buffered just before a worker released ownership.
        if (!working) McpDrainInbound(app.runtime_.mcp);

        if (events[1].revents & POLLIN) FlushOutput(false);
        if (events[2].revents & POLLIN) {
          broker.DrainWake();
          std::string prompt;
          std::string initial;
          bool keep_history = false;
          if (broker.Take(prompt, initial, keep_history)) {
            saved_draft = composer.Buffer();
            answering = true;
            Mount(prompt, initial, keep_history);
          }
        }

        if ((events[0].revents & POLLIN) || composer.HasPending()) {
          InteractiveInputEvent event = composer.Read();
          if (event.kind != InteractiveInputKind::kNone) {
            HandleInputEvent(std::move(event));
          }
        }

        if (interrupting && !AbortRequested()) {
          interrupting = false;
          RefreshStatus();
        }

        if (!working && worker.joinable()) {
          worker.join();
          FlushOutput(true);
          bool activity_ready = app.agent_.DrainBackground();
          app.SaveSession();
          if (worker_quit) exit_when_idle = true;
          interrupting = false;
          if (!exit_when_idle && next_input) {
            StartWork(std::move(*next_input));
            next_input.reset();
          }
          if (activity_ready) Mount();
          RefreshStatus();
        }

        // ProcessSupervisor mirrors activity changes into the broker pipe, so
        // idle completion is handled without a periodic UI tick.
        if (!working && !worker.joinable() && !answering && !exit_when_idle) {
          if (app.agent_.DrainBackground()) {
            app.SaveSession();
            Mount();
            RefreshStatus();
          }
        }

        if (working && !answering) {
          auto now = std::chrono::steady_clock::now();
          std::string current_state = StatusState();
          bool state_changed = current_state != last_state;
          if (state_changed ||
              now - last_redraw >= std::chrono::milliseconds(100)) {
            RefreshStatus();
            last_redraw = now;
            last_state = std::move(current_state);
          }
        }
      }

      if (worker.joinable()) worker.join();
      FlushOutput(true);
      composer.Stop();
      SetInteractiveReadHandler({});
      app.runtime_.processes.SetNotifyFd(-1);
      SetTerminalWakeFd(-1);
      broker.Shutdown();
      SetPersistentComposer(false);
      output.Stop();
      return 0;
    }
  };

  int RunPersistentInteractive() { return InteractiveLoop(*this).Run(); }

  int FinishInteractive(int status) {
    SaveSession();
    Teardown(exit_reason_.c_str());
    TerminalRestore();
    return status;
  }

  int RunInteractive() {
    ResumeAtStartup();
    persist_ = isatty(STDIN_FILENO);
    if (persist_ && AgentDepth() == 0 && api_.config.memory_enabled &&
        api_.config.memory_generate) {
      std::string extractor_error = StartMemoryExtractor(
          runtime_.processes, api_, CanonicalAccessPath(CanonicalCwd()),
          session_file_);
      if (!extractor_error.empty()) {
        DebugLog("memory_extract_start_error", {{"error", extractor_error}});
      }
    }
    if (persist_) {
      int persistent_status = RunPersistentInteractive();
      if (persistent_status >= 0) return FinishInteractive(persistent_status);
    }
    for (;;) {
      SaveSession();
      agent_.DrainBackground();
      PrintStatusBar(
          StatusBar(api_, agent_.SessionUsage(), SessionStatusView(Session())));
      bool eof = false;
      std::string line = ReadInputLine(InputPrompt(), &eof);
      if (eof) {
        if (g_tty) printf("\r\033[2K\r");
        printf("\n");
        break;
      }
      if (ProcessInput(std::move(line))) break;
    }
    return FinishInteractive(0);
  }

  AppContext& context_;
  AppRuntime& runtime_;
  Api& api_;
  Agent& agent_;
  std::vector<Attachment> attachments_;
  std::string session_file_;
  uint64_t saved_revision_;
  bool persist_ = false;
  std::string exit_reason_ = "eof";
};

}  // namespace

int RunApplication(AppContext& context) { return Application(context).Run(); }

}  // namespace uagent
