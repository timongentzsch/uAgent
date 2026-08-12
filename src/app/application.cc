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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/bootstrap.h"
#include "include/app/headless.h"
#include "include/cli.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
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

  void RunTurns(std::string input, json content = nullptr) {
    agent_.Turn(input, std::move(content));
    SteeringState().Take();
  }

  void LogSessionEnd(const char* reason) const {
    if (!Debug().Enabled()) return;
    Debug().Write("session_end", {{"reason", reason},
                                  {"usage", UsageJson(agent_.SessionUsage())},
                                  {"context_tokens", agent_.ContextUsed()}});
  }

  // Release owned processes and per-session files, then close the trace.
  void Teardown(const char* reason) {
    runtime_.Shutdown();
    std::remove(UsageLedger().c_str());
    LogSessionEnd(reason);
  }

  int FinishHeadless(std::string answer, std::string error, int exit_code) {
    Teardown(exit_code == 0 ? "headless_complete" : "headless_error");
    if (context_.options.json_stream || context_.options.json) {
      json envelope = HeadlessResult(
          std::move(answer), std::move(error), agent_.LatestToolTrace(),
          agent_.SessionUsage(), agent_.RouteUsageJson(), exit_code);
      if (context_.options.json_stream) {
        Events().Emit(exit_code == 0 ? "answer" : "error", std::move(envelope));
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
      content = AttachmentContent(context_.options.prompt, attachments_, error);
      if (!error.empty()) {
        context_.output.Restore();
        return FinishHeadless("", std::move(error), 2);
      }
    }
    RunTurns(context_.options.prompt, std::move(content));
    // A background task may finish after the model has yielded prose. Headless
    // mode has no idle REPL to deliver that event later, so keep the process
    // alive and resume from each completion instead of silently killing pending
    // work during runtime shutdown.
    while (runtime_.processes.JoinableCount() > 0 && !AbortRequested()) {
      if (agent_.DrainBackground()) {
        agent_.ContinueAfterActivity();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    if (answer.empty()) {
      std::string error = agent_.LastError().empty()
                              ? "agent produced no answer"
                              : agent_.LastError();
      return FinishHeadless("", std::move(error), 1);
    }
    return FinishHeadless(std::move(answer), "", 0);
  }

  void ResumeAtStartup() {
    if (context_.options.resume_pick) {
      ResumeInto(agent_, PickSession(), session_file_);
    } else if (context_.options.resume_latest) {
      std::vector<SessionInfo> sessions = ListSessions();
      if (sessions.empty()) {
        printf("%s· no saved sessions%s\n", DIM(), RST());
      } else {
        ResumeInto(agent_, sessions.front().path, session_file_);
      }
    }
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
    chmod(directory.c_str(), 0700);
    std::string error;
    if (!agent_.Save(session_file_, error)) {
      fprintf(stderr, "cannot save session: %s\n", error.c_str());
      return;
    }
    saved_revision_ = agent_.Revision();
  }

  bool HandleCommand(const ParsedSlashCommand& command) {
    switch (command.spec->id) {
      case SlashCommandId::kQuit:
        exit_reason_ = "command";
        return true;
      case SlashCommandId::kReset:
        agent_.Reset();
        attachments_.clear();
        session_file_.clear();
        saved_revision_ = agent_.Revision();
        printf("%s· fresh session%s\n", DIM(), RST());
        break;
      case SlashCommandId::kSessions: {
        std::string chosen = PickSession();
        if (!chosen.empty()) {
          ResumeInto(agent_, chosen, session_file_);
          attachments_.clear();
          saved_revision_ = agent_.Revision();
        }
        break;
      }
      case SlashCommandId::kTrace:
        agent_.PrintTrace();
        break;
      case SlashCommandId::kVariant:
        HandleVariant(command.argument);
        break;
      case SlashCommandId::kVerbose:
        agent_.SetVerbose(!agent_.Verbose());
        printf("%s· verbose %s%s\n", DIM(),
               agent_.Verbose() ? "ON — expanded bounded tool output"
                                : "off — compact tool output",
               RST());
        break;
      case SlashCommandId::kHelp:
        PrintCommandHelp();
        break;
      case SlashCommandId::kModels:
        HandleModels(command.argument);
        break;
      case SlashCommandId::kModel:
        HandleModel(command.argument);
        break;
      case SlashCommandId::kEffort:
        HandleEffort(command.argument);
        break;
      case SlashCommandId::kYolo:
        context_.options.yolo = !context_.options.yolo;
        api_.server_tools_authorized = context_.options.yolo;
        printf("%s· yolo %s%s\n", DIM(),
               context_.options.yolo ? "ON — auto-approving everything" : "off",
               RST());
        break;
      case SlashCommandId::kCompact:
        HandleCompact();
        break;
      case SlashCommandId::kContext:
        agent_.PrintContext();
        break;
      case SlashCommandId::kCost:
        HandleCost();
        break;
      case SlashCommandId::kMemory:
        HandleMemory();
        break;
      case SlashCommandId::kAttach:
        HandleAttach(command.argument);
        break;
      case SlashCommandId::kOnline:
        HandleOnline();
        break;
      case SlashCommandId::kProcesses:
        HandleProcesses();
        break;
    }
    return false;
  }

  void HandleModels(const std::string& argument) {
    if (argument.empty()) {
      printf(
          "%s· use /models QUERY to search every provider, or /models all "
          "for the full catalog%s\n",
          DIM(), RST());
      return;
    }
    std::string suffix =
        argument == "all" ? "" : " for " + TerminalSafe(argument);
    printf("%s· searching all model catalogs%s%s\n", DIM(), suffix.c_str(),
           RST());
    TerminalSpinner spinner(true, SpinnerLabel("searching model catalogs"));
    ModelSearch search = SearchModels(api_, context_.provider.routes,
                                      context_.provider.providers, argument);
    spinner.Stop();
    if (AbortRequested()) {
      ClearAbort();
      printf("%s· model search cancelled%s\n", YEL(), RST());
      return;
    }
    std::optional<ModelCandidate> selected = PickModel(search, api_);
    if (!selected) return;
    ApplyRoute(api_, selected->route);
    SaveSelectedModel(selected->selection);
  }

  void HandleCost() const {
    json routes = agent_.RouteUsageJson();
    if (routes.empty()) {
      printf("%s· no session spend yet%s\n", DIM(), RST());
      return;
    }
    for (const auto& [route, usage] : routes.items()) {
      std::string cost = JsonValue(usage, "cost_reported", false)
                             ? FmtCost(JsonValue(usage, "cost", 0.0))
                             : "cost unavailable";
      printf("%s· %s · %s%s\n", DIM(), TerminalSafe(route).c_str(),
             cost.c_str(), RST());
    }
    printf("%s· total %s", DIM(),
           agent_.SessionUsage().cost_reported
               ? FmtCost(agent_.SessionUsage().cost).c_str()
               : "cost unavailable");
    if (api_.config.session_budget > 0) {
      printf(" / %s", FmtCost(api_.config.session_budget).c_str());
    }
    printf("%s\n", RST());
  }

  void HandleMemory() const {
    printf("%s· memory %s%s\n", DIM(),
           api_.config.memory_enabled ? "on" : "off", RST());
    if (!api_.config.memory_enabled) return;
    std::vector<MemoryEntry> entries = ListMemories();
    if (entries.empty()) {
      printf("%s· no saved memories%s\n", DIM(), RST());
      return;
    }
    for (const MemoryEntry& entry : entries) {
      printf("%s· %s · %s%s\n", DIM(), TerminalSafe(entry.key).c_str(),
             TerminalSafe(Tilde(entry.path)).c_str(), RST());
    }
  }

  void HandleModel(const std::string& argument) {
    if (argument.empty()) {
      HandleModels("");
      return;
    }
    std::string selected = SelectModel(api_, context_.provider.routes,
                                       context_.provider.providers, argument);
    if (selected.empty()) {
      printf("%s· unknown model %s; use /models%s\n", RED(),
             TerminalSafe(argument).c_str(), RST());
      return;
    }
    SaveSelectedModel(selected);
  }

  void SaveSelectedModel(const std::string& selected) {
    bool named_route = ResolveModelRoute(context_.provider.routes,
                                         context_.provider.providers, selected)
                           .has_value();
    ActivateCurrentRoute();
    std::string error;
    bool saved =
        SaveModelPreference({selected, api_.base_url, named_route}, error);
    DebugLog("route_changed", {{"route", selected},
                               {"model", api_.model},
                               {"base_url", api_.base_url},
                               {"effort", api_.reasoning_effort},
                               {"preference_saved", saved}});
    printf("%s· model %s%s\n", DIM(),
           ModelLabel(selected, api_.reasoning_effort).c_str(), RST());
    if (!saved) {
      printf("%s· model changed but preference was not saved: %s%s\n", YEL(),
             TerminalSafe(error).c_str(), RST());
    }
  }

  void ActivateCurrentRoute() {
    ActivateRoute(api_);
    agent_.RouteChanged();
  }

  void HandleEffort(const std::string& argument) {
    if (argument.empty()) {
      printf("%s· effort %s%s\n", DIM(),
             api_.reasoning_effort.empty() ? "default"
                                           : api_.reasoning_effort.c_str(),
             RST());
    } else if (argument == "default") {
      api_.reasoning_effort.clear();
      ActivateCurrentRoute();
      printf("%s· effort provider default%s\n", DIM(), RST());
    } else if (!ValidEffort(argument)) {
      printf(
          "%s· effort must be none, minimal, low, medium, high, xhigh, or "
          "max; use default to defer to the provider%s\n",
          RED(), RST());
    } else {
      api_.reasoning_effort = argument;
      ActivateCurrentRoute();
      printf("%s· effort %s%s\n", DIM(), argument.c_str(), RST());
    }
  }

  void HandleVariant(const std::string& argument) {
    if (!api_.openrouter_compatible) {
      printf("%s· /variant is available only for OpenRouter%s\n", RED(), RST());
      return;
    }
    std::string variant = argument;
    if (variant.starts_with(':')) variant.erase(0, 1);
    if (variant.empty()) {
      std::string label = api_.config.openrouter_variant.empty()
                              ? "default"
                              : ":" + api_.config.openrouter_variant;
      printf("%s· variant %s · choose default, nitro, floor, or exacto%s\n",
             DIM(), label.c_str(), RST());
      return;
    }
    if (variant == "default") variant.clear();
    if (!ValidOpenRouterVariant(variant)) {
      printf("%s· variant must be default, nitro, floor, or exacto%s\n", RED(),
             RST());
      return;
    }
    api_.config.openrouter_variant = variant;
    runtime_.config.openrouter_variant = variant;
    setenv("UAGENT_OPENROUTER_VARIANT", variant.c_str(), 1);
    ActivateCurrentRoute();
    const char* detail = "provider default";
    if (variant == "nitro") detail = "highest throughput";
    if (variant == "floor") detail = "lowest price";
    if (variant == "exacto") detail = "quality-first tool reliability";
    DebugLog("variant_changed",
             {{"variant", variant}, {"model", api_.RequestModel()}});
    std::string label = variant.empty() ? "default" : ":" + variant;
    printf("%s· variant %s — %s%s\n", DIM(), label.c_str(), detail, RST());
  }

  void HandleCompact() {
    agent_.Compact();
    SteeringState().Take();
  }

  void HandleAttach(const std::string& argument) {
    if (argument.empty()) {
      if (attachments_.empty()) {
        printf("%s· no pending attachments%s\n", DIM(), RST());
      } else {
        for (const Attachment& attachment : attachments_) {
          printf("%s· %s (%s)%s\n", DIM(),
                 TerminalSafe(attachment.path).c_str(), attachment.mime.c_str(),
                 RST());
        }
      }
      return;
    }
    if (argument == "clear") {
      attachments_.clear();
      printf("%s· attachments cleared%s\n", DIM(), RST());
      return;
    }
    Attachment attachment;
    std::string error;
    if (!InspectAttachment(argument, attachment, error) ||
        !(error = ImageInputError(attachment)).empty()) {
      printf("%s%s%s\n", RED(), error.c_str(), RST());
      return;
    }
    attachments_.push_back(std::move(attachment));
    printf("%s· attached %s for the next message%s\n", DIM(),
           attachments_.back().name.c_str(), RST());
  }

  void HandleOnline() {
    if (!api_.openrouter_compatible) {
      printf("%s· /online is available only for OpenRouter%s\n", RED(), RST());
      return;
    }
    constexpr std::string_view kOnline = ":online";
    bool enabled =
        api_.model.size() > kOnline.size() && api_.model.ends_with(kOnline);
    if (enabled) {
      api_.model.erase(api_.model.size() - kOnline.size());
    } else {
      api_.model += kOnline;
    }
    ActivateCurrentRoute();
    printf("%s· web search %s%s\n", DIM(),
           enabled ? "off"
                   : "ON — ~2K extra input tokens + search fees per request",
           RST());
  }

  void HandleProcesses() const {
    ToolResult activities = ToolActivityList(runtime_.processes);
    if (activities.output.starts_with('(')) {
      printf("%s· %s%s\n", DIM(), activities.output.c_str(), RST());
      return;
    }
    printf("%sbackground work%s\n%s%s%s", BOLD(), RST(), DIM(),
           TerminalSafe(activities.output).c_str(), RST());
  }

  void RunPrompt(std::string input) {
    json content;
    if (!attachments_.empty()) {
      std::string error;
      content = AttachmentContent(input, attachments_, error);
      if (!error.empty()) {
        printf("%s%s%s\n", RED(), error.c_str(), RST());
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
    if (command.spec) return HandleCommand(command);
    if (input[0] == '/') {
      printf("%s· unknown command %s; use /help%s\n", RED(),
             TerminalSafe(input).c_str(), RST());
      return false;
    }
    RunPrompt(std::move(input));
    return false;
  }

  int RunPersistentInteractive() {
    InteractiveOutput output;
    if (!output.Start()) return -1;
    RawComposer composer(output);
    if (!composer.Start()) return -1;
    InputBroker broker;
    SetInteractiveReadHandler([&broker](const std::string& prompt, bool* eof,
                                        bool keep_history,
                                        const std::string& initial) {
      return broker.Read(prompt, eof, keep_history, initial);
    });
    g_persistent_composer = true;

    std::thread worker;
    std::atomic<bool> working{false};
    std::atomic<bool> worker_quit{false};
    bool interrupting = false;
    bool exit_when_idle = false;
    bool answering = false;
    std::optional<std::string> next_input;
    std::string saved_draft;
    std::string pending_output;
    size_t spinner_frame = 0;
    auto started = std::chrono::steady_clock::now();

    auto status = [&] {
      if (!working) {
        return StatusBar(api_, agent_, context_.options.yolo,
                         attachments_.size(), runtime_.processes);
      }
      double elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - started)
                           .count();
      size_t background = runtime_.processes.Count();
      char seconds[32];
      snprintf(seconds, sizeof seconds, "%.1fs", elapsed);
      std::string activity = CurrentTerminalActivity();
      static constexpr const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                "⠴", "⠦", "⠧", "⠇", "⠏"};
      std::string line = kFrames[spinner_frame % 10];
      line += " ";
      line += interrupting ? "interrupting"
                           : (activity.empty() ? "working" : activity);
      line += " · " + std::string(seconds);
      line += " · ctx " + FmtCount(agent_.ContextSnapshot());
      if (background > 0) line += " · bg:" + std::to_string(background);
      if (SteeringEnabled()) line += " · Esc interrupt";
      size_t queued = SteeringState().QueuedCount();
      if (queued > 0) {
        line += " · steer:" + std::to_string(queued);
      }
      return line;
    };

    auto rendered_status = [&] { return StatusBarLine(status()); };

    auto status_state = [&] {
      return std::string(interrupting ? "interrupting|" : "working|") +
             CurrentTerminalActivity() + "|" +
             std::to_string(SteeringState().QueuedCount()) + "|" +
             std::to_string(runtime_.processes.Count());
    };

    auto unmount = [&] {
      if (!composer.Drawn()) return;
      output.Write("\r\033[" + std::to_string(composer.CaretRow() + 1) +
                   "A\033[J");
      composer.Detach();
    };

    auto mount = [&](const std::string& prompt = InputPrompt(),
                     const std::string& initial = std::string(),
                     bool keep_history = true) {
      unmount();
      output.Write(rendered_status() + "\n");
      composer.Mount(prompt, initial, keep_history);
    };

    auto insert = [&](std::string text) {
      if (text.empty()) return;
      if (text.back() != '\n') text += '\n';
      unmount();
      output.Write(text);
      output.Write(rendered_status() + "\n");
      composer.Remount();
    };

    auto redraw = [&] {
      unmount();
      output.Write(rendered_status() + "\n");
      composer.Remount();
    };

    auto refresh_status = [&] {
      if (!composer.Drawn()) return;
      // The status is always the one row immediately above the mounted
      // composer; update it without repainting the editor's footprint.
      size_t rows_up = composer.CaretRow() + 1;
      output.Write("\r\033[" + std::to_string(rows_up) + "A");
      output.Write(rendered_status());
      output.Write("\033[" + std::to_string(rows_up) + "B\r");
      if (composer.CaretColumn() > 0) {
        output.Write("\033[" + std::to_string(composer.CaretColumn()) + "C");
      }
    };

    auto flush_output = [&](bool all) {
      pending_output += output.Read();
      size_t split = all ? pending_output.size() : pending_output.rfind('\n');
      if (split == std::string::npos || split == 0) return;
      if (!all) ++split;
      std::string ready = pending_output.substr(0, split);
      pending_output.erase(0, split);
      insert(std::move(ready));
    };

    auto start_work = [&](std::optional<std::string> input) {
      agent_.ContextUsed();
      working = true;
      worker_quit = false;
      interrupting = false;
      spinner_frame = 0;
      started = std::chrono::steady_clock::now();
      worker = std::thread([&, input = std::move(input)]() mutable {
        if (input) {
          worker_quit = ProcessInput(std::move(*input));
        } else {
          agent_.ContinueAfterActivity();
        }
        working = false;
        broker.Notify();
      });
    };

    mount();
    auto last_redraw = std::chrono::steady_clock::now();
    std::string last_state = status_state();
    while (!exit_when_idle || working) {
      pollfd events[3] = {{STDIN_FILENO, POLLIN, 0},
                          {output.Fd(), POLLIN, 0},
                          {broker.Fd(), POLLIN, 0}};
      int ready = poll(events, 3, 100);
      if (ready < 0 && errno != EINTR) break;
      if (g_terminal_resized) {
        g_terminal_resized = 0;
        redraw();
        last_redraw = std::chrono::steady_clock::now();
        last_state = status_state();
      }

      // Keep cooperative MCP requests responsive while the composer is idle.
      // The worker owns MCP transports during a turn, so the two never race.
      if (!working) McpDrainInbound(runtime_.mcp);

      if (events[1].revents & POLLIN) flush_output(false);
      if (events[2].revents & POLLIN) {
        broker.DrainWake();
        std::string prompt;
        std::string initial;
        bool keep_history = false;
        if (broker.Take(prompt, initial, keep_history)) {
          saved_draft = composer.Buffer();
          answering = true;
          mount(prompt, initial, keep_history);
        }
      }

      if ((events[0].revents & POLLIN) || composer.HasPending()) {
        InteractiveInputEvent event = composer.Read();
        if (event.kind != InteractiveInputKind::kNone) {
          if (answering) {
            bool eof = event.kind == InteractiveInputKind::kEscape ||
                       event.kind == InteractiveInputKind::kEof;
            broker.Answer(std::move(event.text), eof);
            answering = false;
            mount(InputPrompt(), saved_draft);
          } else if (event.kind == InteractiveInputKind::kEscape) {
            // A bare Escape clears the current input line (and any stashed
            // draft). Still honour a steering/interrupt request if the agent is
            // working, so Esc doubles as the interrupt key.
            saved_draft.clear();
            composer.Clear();
            if (working && SteeringEnabled() && !interrupting) {
              interrupting = true;
              SteeringState().Request();
              refresh_status();
            }
          } else if (event.kind == InteractiveInputKind::kEof) {
            exit_when_idle = true;
            mount();
          } else {
            std::string input = Trim(event.text);
            if (!input.empty()) {
              if ((input == "/q" || input == "/quit") && working) {
                exit_when_idle = true;
              } else if (working) {
                SteeringState().Queue(std::move(input));
              } else if (worker.joinable()) {
                next_input = std::move(input);
              } else {
                start_work(std::move(input));
              }
            }
            mount();
          }
        }
      }

      if (!working && worker.joinable()) {
        worker.join();
        flush_output(true);
        SaveSession();
        bool activity_ready = agent_.DrainBackground();
        if (worker_quit) exit_when_idle = true;
        interrupting = false;
        if (!exit_when_idle && next_input) {
          start_work(std::move(*next_input));
          next_input.reset();
        } else if (!exit_when_idle && activity_ready) {
          start_work(std::nullopt);
        }
        refresh_status();
      }

      // A task or command can finish after the parent has returned prose. Fold
      // its result into the conversation and resume the idle agent directly;
      // the existing 100 ms UI poll is the event source, so no watcher thread
      // or second process owner is needed.
      if (!working && !worker.joinable() && !answering && !exit_when_idle) {
        if (agent_.DrainBackground()) {
          start_work(std::nullopt);
          refresh_status();
        }
      }

      if (working && !answering) {
        auto now = std::chrono::steady_clock::now();
        std::string current_state = status_state();
        bool state_changed = current_state != last_state;
        if (state_changed ||
            now - last_redraw >= std::chrono::milliseconds(200)) {
          ++spinner_frame;
          refresh_status();
          last_redraw = now;
          last_state = std::move(current_state);
        }
      }
    }

    if (worker.joinable()) worker.join();
    flush_output(true);
    composer.Stop();
    SetInteractiveReadHandler({});
    broker.Shutdown();
    g_persistent_composer = false;
    output.Stop();
    return 0;
  }

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
      PrintStatusBar(StatusBar(api_, agent_, context_.options.yolo,
                               attachments_.size(), runtime_.processes));
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
