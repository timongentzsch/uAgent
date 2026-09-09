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
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
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
#include "include/tools/child_agent.h"
#include "include/tools/jobs.h"
#include "include/tools/memory.h"
#include "include/tools/process.h"
#include "include/tools/subagent.h"
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
        session_file_(context.channel ? context.channel->SessionPath()
                                      : CollaboratorSessionFile()),
        saved_revision_(agent_.Revision()),
        channel_(context.channel) {
    agent_.RetainExchanges(channel_ || context_.options.prompt.empty() ||
                           !session_file_.empty());
    message_subscription_ =
        context_.observability.Subscribe([this](const AppEvent& event) {
          if (event.type != "message.changed" ||
              (!persist_ && session_file_.empty())) {
            return;
          }
          std::string kind = JsonValue(event.data["block"], "kind", "");
          if (kind == "user" || kind == "attachment") SaveSession(true);
        });
  }
  ~Application() { context_.observability.Unsubscribe(message_subscription_); }

  AppSession Session() {
    return AppSession{context_, attachments_, session_file_, saved_revision_};
  }

  int Run() {
    int attachment_status = LoadInitialAttachments();
    if (attachment_status != 0) return attachment_status;
    if (!context_.options.prompt.empty() &&
        (context_.options.resume_latest || !session_file_.empty())) {
      if (!ResumeAtStartup()) return 2;
    }
    if (!context_.options.prompt.empty()) return RunHeadless();
    return channel_ ? RunChannel() : RunInteractive();
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

  void RunTurns(const std::string& input, json content = nullptr,
                json images = json::array()) {
    EnsureSessionPath();
    ReloadConfigAtTurnBoundary();
    bool was_automatic = ApprovalIsAutomatic();
    PermissionControl(context_, json::object());
    if (was_automatic != ApprovalIsAutomatic()) agent_.ApprovalChanged();
    agent_.Turn(input, std::move(content), std::move(images), request_id_);
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
      json envelope =
          HeadlessResult(std::move(answer), std::move(error),
                         agent_.LatestToolTrace(), agent_.SessionUsage(),
                         agent_.RouteUsageJson(), exit_code, agent_.LastStop());
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
    // Internal collaborator sessions are explicit durable conversations even
    // though ordinary one-shot `-p` calls remain ephemeral.
    persist_ = !session_file_.empty();
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
    SaveSession();
    // Background work is observational and never starts a model turn. Keep the
    // process alive long enough to publish completion and drain retained state.
    while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
      uint64_t generation = runtime_.processes.Generation();
      if (!agent_.DrainBackground()) {
        runtime_.processes.WaitForChange(generation);
      }
      SaveSession();
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

  bool ResumeAtStartup() {
    std::string previous_path = session_file_;
    if (!session_file_.empty()) {
      if (PathExists(session_file_)) {
        if (!ResumeInto(agent_, session_file_, session_file_, !channel_)) {
          return false;
        }
      }
    } else if (context_.options.resume_pick) {
      std::string path = PickSession();
      if (!path.empty() && !ResumeInto(agent_, path, session_file_)) {
        return false;
      }
    } else if (context_.options.resume_latest) {
      std::vector<SessionInfo> sessions = ListSessions();
      if (sessions.empty()) {
        printf("%s· no saved sessions%s\n", DIM(), RST());
        fflush(stdout);
      } else {
        if (!ResumeInto(agent_, sessions.front().path, session_file_)) {
          return false;
        }
      }
    }
    AppSession session = Session();
    LoadSessionJournal(session, previous_path);
    saved_revision_ = agent_.Revision();
    return true;
  }

  void SaveSession(bool force = false) {
    auto session = Session();
    SaveSessionSettings(session);
    if ((!persist_ && session_file_.empty()) ||
        (!force && agent_.MessageCount() <= 1) ||
        agent_.Revision() == saved_revision_) {
      return;
    }
    EnsureSessionPath();
    std::string error;
    if (!agent_.Save(session_file_, error) ||
        !context_.observability.Journal().Flush(session_file_ + ".events.jsonl",
                                                error)) {
      input_error_ = "cannot save session: " + error;
      fprintf(stderr, "%s\n", input_error_.c_str());
      Emit(Event{EventId::kError, {{"error", input_error_}}});
      return;
    }
    saved_revision_ = agent_.Revision();
  }

  void EnsureSessionPath() {
    if (persist_ && session_file_.empty()) {
      session_file_ =
          UagentDir(kHistoryDir) + "/" + WorkspaceId(CanonicalCwd()) + "/" +
          UtcStamp("%Y%m%dT%H%M%SZ") + "-" + MakeSessionId() + ".json";
    }
    runtime_.processes.SetOwner(session_file_.empty() ? agent_.SessionId()
                                                      : HashHex(session_file_));
  }

  // An improvement round ends by reinstalling, and this session is still the
  // old build: nothing it reports about itself is true of the new one.
  void ReportReplacedExecutable() {
    if (!ExecutableReplaced()) return;
    context_.observability.Emit(NoticeEvent(
        PresentationStatus::kNeutral,
        "· uagent was replaced on disk; restart to run the new build"));
  }

  void RunPrompt(const std::string& input) {
    ReportReplacedExecutable();
    input_error_.clear();
    json content;
    json images = json::array();
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(
          input, attachments_, error, api_.capabilities.image_input,
          !api_.config.image_model.empty(), api_.capabilities.file_input);
      if (!error.empty()) {
        input_error_ = error;
        if (!channel_) {
          printf("%s%s%s\n", RED(), TerminalSafe(error).c_str(), RST());
          fflush(stdout);
        }
        Emit(Event{EventId::kError, {{"error", error}}});
        return;
      }
      for (const auto& attachment : attachments_) {
        if (!attachment.asset_id.empty()) {
          images.push_back({{"id", attachment.asset_id},
                            {"name", attachment.name},
                            {"mime", attachment.mime},
                            {"bytes", attachment.bytes},
                            {"image", attachment.image},
                            {"path", attachment.path}});
        }
      }
      attachments_.clear();
    }
    RunTurns(input, std::move(content), std::move(images));
  }

  json InterfaceState() const {
    return {{"route", RouteSelection(api_, context_.provider.providers)},
            {"effort", api_.reasoning_effort},
            {"variant", api_.config.openrouter_variant},
            {"context_tokens", agent_.ContextUsed()},
            {"context_window", api_.ctx_window},
            {"attachments", attachments_.size()},
            {"background", runtime_.processes.Count()},
            {"tools", context_.tools.size()},
            {"verbose", agent_.Verbose()},
            {"yolo", ApprovalIsAutomatic()}};
  }

  bool ProcessInput(std::string input) {
    input = Trim(input);
    if (input.empty()) {
      if (!attachments_.empty()) RunPrompt(input);
      return false;
    }
    ParsedSlashCommand command = ParseSlashCommand(input);
    if (command.spec) DebugLog("command", {{"command", command.spec->name}});
    if (std::string prompt = SlashCommandPrompt(command); !prompt.empty()) {
      RunPrompt(prompt);
      return false;
    }
    if (command.spec) {
      AppSession session = Session();
      json result;
      bool quit = RunSlashCommand(session, command, result);
      Emit(Event{EventId::kCommandCompleted,
                 {{"command", command.spec->name},
                  {"argument", command.argument},
                  {"quit", quit},
                  {"result", std::move(result)},
                  {"state", InterfaceState()}}});
      if (!quit) return false;
      exit_reason_ = "command";
      return true;
    }
    if (input[0] == '/') {
      Emit(NoticeEvent(PresentationStatus::kFailed,
                       "· unknown command " + input + "; use /help"));
      return false;
    }
    RunPrompt(input);
    return false;
  }

  // The pinned-region state machine of the persistent composer.
  struct InteractiveLoop {
    explicit InteractiveLoop(Application& owner)
        : app(owner), composer(output) {}

    std::string Status() {
      if (quit_hint) return "ctrl+c again to quit";
      if (!working) {
        return StatusBar(app.api_, app.agent_.SessionUsage(),
                         SessionStatusView(app.Session()));
      }
      ActivityView view;
      view.elapsed = std::chrono::steady_clock::now() - started;
      view.context_used = app.agent_.ContextSnapshot();
      view.context_window = app.api_.ctx_window;
      AppSession session = app.Session();
      view.model = RouteSelection(session.ApiClient(),
                                  session.context.provider.providers);
      view.subagent = SubagentProgress(&view.subagents);
      size_t running = app.runtime_.processes.Count();
      // The children have their own chip, so `bg:` is what is left: two
      // snapshots a moment apart can disagree, and a saturating subtraction is
      // the difference between a stale number and an absurd one.
      view.background = running > view.subagents ? running - view.subagents : 0;
      view.foreground = app.runtime_.processes.ForegroundCount();
      view.queued = SteeringState().QueuedCount();
      view.interrupting = interrupting;
      return ActivityBar(view);
    }

    // The newest child that has said something, as "<id>: <progress>", and the
    // number of children beside it. One snapshot feeds both, so the count and
    // the line can never describe different sets. A child that has not printed
    // yet is skipped rather than shown blank -- the row falls back to the
    // label it would otherwise have had.
    std::string SubagentProgress(size_t* count) {
      std::vector<SubagentView> agents = app.runtime_.processes.SubagentViews();
      *count = agents.size();
      for (auto it = agents.rbegin(); it != agents.rend(); ++it) {
        if (it->tail.empty()) continue;
        std::string id = it->source_id.empty() ? "#" + std::to_string(it->id)
                                               : it->source_id;
        return id + ": " + it->tail;
      }
      return std::string();
    }

    std::string RenderedStatus() {
      return StatusBarLine(Status(), &status_columns);
    }

    // Everything the working row shows that is not the clock. The children's
    // progress is part of it: without that, a repaint would wait for the next
    // hundred-millisecond tick and the line would lag what the child said.
    std::string StatusState() {
      size_t agents = 0;
      std::string progress = SubagentProgress(&agents);
      return std::string(interrupting ? "interrupting|" : "working|") +
             CurrentTerminalActivity() + "|" +
             std::to_string(SteeringState().QueuedCount()) + "|" +
             std::to_string(app.runtime_.processes.Count()) + "|" +
             std::to_string(app.runtime_.processes.ForegroundCount()) + "|" +
             std::to_string(agents) + "|" + progress;
    }

    size_t TailRows() const {
      return DisplayRows(output_tail, TerminalWidth());
    }

    // Erase from the top of the pinned region, which after a narrowing resize
    // starts above the status row: a terminal that rewraps has turned the row
    // written at the old width into several, and erasing from the last one
    // would leave the rest on screen for every later repaint to add to.
    void Unmount() {
      if (!composer.Drawn()) return;
      size_t rows_up = composer.CaretRow() + 1 + TailRows() +
                       StatusOverflowRows(status_columns, TerminalWidth());
      output.Write("\r\033[" + std::to_string(rows_up) + "A\033[J");
      composer.Detach();
    }

    // The one place the pinned region is painted: erase what is there, emit any
    // transcript text above it, then the status row and the composer. Every
    // caller differs only in the text it contributes and whether the composer
    // keeps its buffer, so geometry can only be wrong here.
    void Paint(std::string text, const std::string* prompt,
               const std::string& initial, bool keep_history,
               std::optional<std::string> tail = std::nullopt) {
      Unmount();
      if (tail) output_tail = std::move(*tail);
      if (!text.empty()) {
        if (text.back() != '\n') text += '\n';
        output.Write(text);
      }
      if (!output_tail.empty()) output.Write(output_tail + "\n");
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
      InteractiveOutputUpdate update = output.Read(all);
      if (!update.changed) return;
      if (update.adopted_prefix_bytes > 0) {
        output_tail.clear();
        update.committed.erase(
            0, std::min(update.adopted_prefix_bytes, update.committed.size()));
        if (update.committed.empty() && update.tail.empty()) return;
      }
      Paint(std::move(update.committed), nullptr, {}, true,
            std::move(update.tail));
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
    bool quit_hint = false;
    bool answering = false;
    std::deque<std::string> next_inputs;
    std::string saved_draft;
    std::string output_tail;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> resize_settled;
    // The width the pinned status row occupies on screen. iTerm2 and every
    // other terminal that rewraps on resize turns a row written at a wider
    // terminal into several physical rows, and the erase in Unmount() has to
    // walk over all of them.
    size_t status_columns = 0;

    void HandleInputEvent(InteractiveInputEvent event) {
      quit_hint = false;  // any other key ends the quit gesture
      if (event.kind == InteractiveInputKind::kLine && !answering) {
        // Submission has already printed the prompt below the status row.
        // Replace both regions so the transient working row does not enter
        // scrollback above steering or ordinary user input.
        size_t rows_up = composer.LastSubmittedRows() + 1;
        output.Write("\r\033[" + std::to_string(rows_up) + "A\033[J");
        output.Write(UserEchoRow(composer.Prompt(), TerminalSafe(event.text)) +
                     "\n");
        // The submitted row now follows the visible tail, so that tail has
        // entered scrollback and later deltas must start below the user row.
        if (!output_tail.empty()) {
          output.AdoptTail();
          output_tail.clear();
        }
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
          ParsedSlashCommand command = ParseSlashCommand(input);
          if (working && command.spec &&
              (command.spec->id == SlashCommandId::kProcesses ||
               command.spec->id == SlashCommandId::kAgents)) {
            std::istringstream activity_input(command.argument);
            std::string target, operation;
            activity_input >> target >> operation;
            if (operation == "followup") {
              output.Write("Follow-up requires an idle foreground turn.\n");
            } else {
              AppSession session = app.Session();
              output.Write(TerminalSafe(
                  ActivityText(ActivityCommand(session, command))));
            }
            Mount();
            RefreshStatus();
            return;
          }
          if (working && command.spec &&
              command.spec->id == SlashCommandId::kPermissions) {
            output.Write(TerminalSafe(JsonDump(PermissionControl(
                             app.context_, {{"mode", command.argument}}))) +
                         "\n");
            Mount();
            return;
          }
          if (working && command.spec &&
              command.spec->id != SlashCommandId::kQuit) {
            if (next_inputs.size() < 8) {
              next_inputs.push_back(std::move(input));
              output.Write("Command queued until the current work finishes.\n");
            } else {
              output.Write("Command queue is full; retry when idle.\n");
            }
            Mount();
            return;
          }
          if ((input == "/q" || input == "/quit") && working) {
            exit_when_idle = true;
          } else if (working) {
            // Publish guidance before waking passive tool waits. Their
            // generation predicate makes this pairing lost-wakeup-safe.
            SteeringState().Queue(std::move(input));
            app.runtime_.processes.Wake();
          } else if (worker.joinable()) {
            next_inputs.push_back(std::move(input));
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
      SetInteractiveReadHandler(
          [this](const InteractionRequest& request, bool* eof) {
            return broker.Read(request.prompt, eof, request.keep_history,
                               request.initial);
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
                  {output.ReadFd(), POLLIN, 0},
                  {broker.ReadFd(), POLLIN, 0}};
        if (!working) {
          for (const auto& server : app.runtime_.mcp.Servers()) {
            if (server->alive && server->out &&
                server->startup == McpStartupState::kReady) {
              events.push_back(
                  {server->out.Get(),
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

        // Idle is the only state where SIGINT asks rather than kills.
        SetQuitGesture(!working && !answering);
        int ready =
            poll(events.data(), static_cast<nfds_t>(events.size()), timeout_ms);
        if (ready < 0 && errno != EINTR) break;
        SetQuitGesture(false);
        if (TakeIdleInterrupt() && !working) {
          // The row is the contract: while it offers the exit, the next press
          // takes it. Any other key retracts the offer. Leaving through
          // raise() is how an unhandled SIGINT always left, so the terminal is
          // restored the same way and the shell still sees 130.
          if (quit_hint) raise(SIGINT);
          quit_hint = true;
          RefreshStatus();
        }
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
          // only downward leaves the status row above the cursor, so every
          // repaint would append another one. Walk up to it first.
          //
          // StatusOverflowRows() accounts for the status row's own rewrap and
          // CaretRow() for the rest, exactly when the composer did not reflow.
          // Narrowing with a soft-wrapped draft can still leave one stale
          // fragment above; the next mount clears it. Fixing that needs a
          // cursor position report.
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
          // Guidance the finished work never read is still something the user
          // typed. `working` stays true until the worker returns, so a line
          // submitted the moment a slash command prints its result is queued
          // as steering for work that never looks at the queue, and a turn
          // already past its last steering check is on the same footing.
          // Promoting it here is the difference between running late and
          // being dropped with the status bar still counting it.
          if (!exit_when_idle) {
            std::string promoted = TakeStrandedSteering();
            if (!promoted.empty()) {
              // Preserve command boundaries: a deferred slash command must
              // never be concatenated into a model prompt.
              next_inputs.push_front(std::move(promoted));
            }
          }
          if (!exit_when_idle && !next_inputs.empty()) {
            std::string next = std::move(next_inputs.front());
            next_inputs.pop_front();
            StartWork(std::move(next));
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
    if (!ResumeAtStartup()) return FinishInteractive(2);
    persist_ = isatty(STDIN_FILENO);
    EnsureSessionPath();
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

  int RunChannel() {
    if (!ResumeAtStartup()) return FinishInteractive(2);
    persist_ = true;
    EnsureSessionPath();
    if (!PathExists(session_file_) && !channel_->InitialTitle().empty()) {
      agent_.Rename(channel_->InitialTitle());
      SaveSession(true);
    }
    runtime_.processes.SetNotifyFd(channel_->WakeFd());
    channel_->SetActivityControl([this](const json& request) {
      if (JsonValue(request, "kind", "") == "permissions") {
        return PermissionControl(context_, request);
      }
      return ActivityControl(runtime_.processes, request);
    });
    PublishChannelState();
    while (std::optional<ApplicationInput> input = channel_->NextInput()) {
      agent_.DrainBackground();
      bool quit = false;
      request_id_ = input->request_id;
      if (input->title) {
        agent_.Rename(std::move(*input->title));
      } else if (!input->control.is_null()) {
        AppSession session = Session();
        json result = SessionControl(session, input->control);
        SaveSession(true);
        channel_->CompleteControl(input->request_id, result);
      } else if (!input->wake) {
        attachments_ = std::move(input->attachments);
        quit = ProcessInput(std::move(input->text));
      }
      SaveSession(input->title.has_value());
      PublishChannelState();
      if (quit) break;
    }
    runtime_.processes.SetNotifyFd(-1);
    channel_->SetActivityControl({});
    return FinishInteractive(0);
  }

  void PublishChannelState() {
    json state = InterfaceState();
    state["view"] = agent_.DisplaySnapshot();
    state["usage"] = UsageJson(agent_.SessionUsage());
    state["statistics"] = agent_.Statistics();
    state["http"] = agent_.HttpExchanges();
    state["permissions"] = PermissionControl(context_, json::object());
    state["efforts"] = json::array({"default"});
    for (const char* effort : kReasoningEfforts) {
      if (SupportsReasoningEffort(api_, effort)) {
        state["efforts"].push_back(effort);
      }
    }
    state["variants"] = json::array();
    if (api_.capabilities.model_variants) {
      state["variants"].push_back("default");
      for (std::string_view variant : kOpenRouterVariants) {
        state["variants"].push_back(variant);
      }
    }
    state["turns"] = agent_.UserTurns();
    state["activities"] = runtime_.processes.ActivityViews();
    state["collaborators"] = CollaboratorSummaries(runtime_.processes);
    state["error"] = input_error_.empty() ? agent_.LastError() : input_error_;
    state["title"] = Utf8Prefix(agent_.FirstUserText(), 256);
    state["stop"] = agent_.LastStop();
    channel_->PublishState(state);
  }

  AppContext& context_;
  AppRuntime& runtime_;
  Api& api_;
  Agent& agent_;
  std::vector<Attachment> attachments_;
  std::string session_file_;
  uint64_t saved_revision_;
  bool persist_ = false;
  std::string request_id_;
  uint64_t message_subscription_ = 0;
  std::string exit_reason_ = "eof";
  std::string input_error_;
  ApplicationChannel* channel_ = nullptr;
};

}  // namespace

int RunApplication(AppContext& context) { return Application(context).Run(); }

}  // namespace uagent
