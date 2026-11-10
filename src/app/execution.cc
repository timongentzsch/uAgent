// Copyright 2026 Timon Gentzsch
#include "include/app/execution.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "include/agent/activity.h"
#include "include/agent/activity_supervisor.h"
#include "include/agent/file_services.h"
#include "include/agent/path_policy.h"
#include "include/app/commands.h"
#include "include/app/folder_overview.h"
#include "include/app/schedule.h"
#include "include/app/terminal.h"
#include "include/app/workspace_files.h"
#include "include/core/build.h"
#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"
#include "include/media/attachments.h"
#include "src/app/execution_internal.h"
#include "src/tools/registry_internal.h"

namespace uagent::execution {
namespace {
using Clock = std::chrono::steady_clock;
struct CallState {
  std::atomic<bool> cancel{false};
  bool done = false;
  json result;
};
class Worker {
 public:
  ~Worker() {
    for (const json& record : DetachedRecords())
      ToolActivityStop(processes_, JsonValue(record, "pid", int64_t{0}));
    ActivityShutdownAll(processes_);
  }
  void CancelAll() {
    std::lock_guard lock(mutex_);
    for (const auto& [id, call] : calls_) call->cancel = true;
    processes_.Wake();
    WakeProcessWaits();
  }
  bool HasChildren() {
    std::lock_guard lock(terminal_mutex_);
    return processes_.Count() > 0 || (terminals_ && terminals_->Running()) ||
           std::ranges::any_of(DetachedRecords(), [](const json& row) {
             return JsonValue(row, "_alive", false);
           });
  }
  json Request(const json& request) {
    const std::string operation = JsonValue(request, "op", "");
    const std::string id = JsonValue(request, "id", "");
    if (operation == "cancel" || operation == "result") {
      std::lock_guard lock(mutex_);
      const auto it = calls_.find(id);
      if (it == calls_.end()) return {{"known", false}};
      if (operation == "cancel") {
        it->second->cancel = true;
        processes_.Wake();
        WakeProcessWaits();
      }
      return {{"known", true},
              {"done", it->second->done},
              {"result", it->second->result}};
    }

    if (id.empty()) return Execute(request);
    if (!session::OpaqueId(id)) return {{"error", "Invalid request identity."}};
    std::shared_ptr<CallState> call;
    {
      std::lock_guard lock(mutex_);
      if (const auto it = calls_.find(id); it != calls_.end())
        return it->second->done ? it->second->result : json{{"pending", true}};
      if (calls_.size() >= session::kReceiptEntries) {
        auto old =
            std::find_if(calls_.begin(), calls_.end(),
                         [](const auto& entry) { return entry.second->done; });
        if (old == calls_.end())
          return {{"error", "Remote execution is busy."}};
        calls_.erase(old);
      }
      call = std::make_shared<CallState>();
      calls_[id] = call;
    }
    LocalAbort abort(call->cancel);
    json result = Execute(request);
    {
      std::lock_guard lock(mutex_);
      call->result = result;
      call->done = true;
    }
    return result;
  }

 private:
  json Scope(const Tool& tool, const json& args) {
    json subject = tool.approval_subject ? tool.approval_subject(args) : args;
    if (const auto* path = JsonStringRef(args, "path"))
      subject = {{"path", *path},
                 {"resolved", CanonicalAccessPath(*path).string()}};
    return {{"subject", subject},
            {"approval", static_cast<int>(RequiredApproval(tool, args))},
            {"outside", tool.needs_approval && tool.needs_approval(args)}};
  }
  json Execute(const json& request) {
    const std::string operation = JsonValue(request, "op", "");
    if (operation == "terminal") {
      const std::string action = JsonValue(request, "action", "");
      if (action == "open") {
        std::lock_guard lock(terminal_mutex_);
        if (!terminals_)
          terminals_ = std::make_unique<Terminals>(16, std::chrono::hours(24));
        std::string error;
        const std::string id =
            terminals_->Open(CanonicalCwd(), CanonicalCwd(), error);
        return error.empty() ? json{{"terminal", id}} : json{{"error", error}};
      }
      std::shared_ptr<TerminalViewer> viewer;
      const std::string token = JsonValue(request, "viewer", "");
      {
        std::lock_guard lock(terminal_mutex_);
        if (!terminals_) return {{"error", "Remote terminal was interrupted."}};
        if (action == "close")
          return terminals_->Close(JsonValue(request, "terminal", ""))
                     ? json::object()
                     : json{{"error", "Terminal has ended."}};
        if (action == "rename") {
          const std::string error =
              terminals_->Rename(JsonValue(request, "terminal", ""),
                                 JsonValue(request, "title", ""));
          return error.empty() ? json::object() : json{{"error", error}};
        }
        if (!session::OpaqueId(token))
          return {{"error", "Invalid terminal viewer."}};
        if (action == "attach") {
          auto attached =
              terminals_->Attach(JsonValue(request, "terminal", ""));
          if (!attached) return {{"error", "Remote terminal was interrupted."}};
          viewers_[token] = {std::move(attached), Clock::now()};
          return json::object();
        }
        const auto it = viewers_.find(token);
        if (it == viewers_.end())
          return {{"error", "Terminal viewer has ended."}};
        if (action == "detach") {
          viewers_.erase(it);
          return json::object();
        }
        viewer = it->second.viewer;
        it->second.touched = Clock::now();
      }
      if (action == "read") {
        const auto out = viewer->Read(std::chrono::seconds(1));
        return {{"data", HexEncode(out.bytes)},
                {"ended", out.ended},
                {"replaced", out.replaced},
                {"code", out.code}};
      }
      if (action == "write") {
        std::string bytes;
        return HexDecode(JsonValue(request, "data", ""), bytes, KiB(64)) &&
                       viewer->Write(bytes)
                   ? json::object()
                   : json{{"error", "Cannot write to terminal."}};
      }
      if (action == "resize") {
        const auto rows = JsonValue(request, "rows", uint16_t{0}),
                   cols = JsonValue(request, "cols", uint16_t{0});
        return rows && cols && rows <= kPtyMaxDimension &&
                       cols <= kPtyMaxDimension && viewer->Resize(rows, cols)
                   ? json::object()
                   : json{{"error", "Invalid terminal dimensions."}};
      }
      return {{"error", "Unknown terminal action."}};
    }
    if (operation == "activity_control")
      return ActivityControl(processes_,
                             request.value("request", json::object()));
    if (operation == "shutdown") {
      for (const json& record : DetachedRecords())
        ToolActivityStop(processes_, JsonValue(record, "pid", int64_t{0}));
      ActivityShutdownAll(processes_);
      return json::object();
    }
    if (operation == "activities")
      return {{"views", processes_.ActivityViews()},
              {"pending", processes_.PendingCount()},
              {"count", processes_.Count()},
              {"generation", processes_.Generation()}};
    if (operation == "completions") {
      json rows = json::array();
      for (const auto& c : ActivityTakeCompletedDetails(processes_))
        rows.push_back({{"activity_id", c.activity_id},
                        {"status", c.status},
                        {"command", c.command},
                        {"output", c.output},
                        {"hint", c.hint},
                        {"display_label", c.display_label},
                        {"started_ms", c.started_ms}});
      ToolResult stopped;
      stopped.ended = processes_.TakeStopped();
      return {{"completions", rows}, {"stopped", EncodeResult(stopped)}};
    }
    if (operation == "ping")
      return {{"build", BuildIdentity()}, {"cwd", CanonicalCwd()}};
    if (operation == "instructions") {
      const std::string action = JsonValue(request, "action", "load");
      if (action == "path")
        return {{"path", InstructionPath(true, CanonicalCwd()).string()}};
      if (action == "files") return InstructionFiles(CanonicalCwd());
      if (action == "write") {
        const auto base = JsonStringRef(request, "base");
        const std::string error = WriteInstructionFile(
            true, CanonicalCwd(), JsonValue(request, "text", ""),
            base ? std::optional<std::string>(*base) : std::nullopt);
        return error.empty() ? json::object() : json{{"error", error}};
      }
      auto instructions =
          LoadProjectInstructions(CanonicalCwd(), ProjectDocBytes());
      return {{"text", instructions.text},
              {"sources", instructions.sources},
              {"truncated", instructions.truncated},
              {"root", ProjectRoot(CanonicalCwd()).string()},
              {"git", InGitRepository(CanonicalCwd())}};
    }
    if (operation == "folder") return FolderGitFacts(CanonicalCwd());
    if (operation == "schedule_check") {
      ApprovalMode mode;
      if (!ParseApprovalMode(JsonValue(request, "mode", "ask"), mode))
        return {{"error", "Invalid task permissions."}};
      SetApprovalMode(mode);
      return ScheduleCheck(request);
    }
    if (operation == "files")
      return workspace_files::Request(CanonicalCwd(), request);
    if (operation == "bytes") {
      const std::string path = JsonValue(request, "path", "");
      if (HiddenPath(path)) return {{"error", "Protected remote path."}};
      std::string bytes, error;
      if (!ReadRegularFile(path, MiB(8), bytes, error))
        return {{"error", error}};
      return {{"data", HexEncode(bytes)}};
    }
    if (operation == "undo") {
      const std::string path = JsonValue(request, "path", "");
      if (HiddenPath(path)) return {{"error", "Protected remote path."}};
      std::string current, error;
      const std::string hash = JsonValue(request, "hash", "");
      if (hash.empty()
              ? PathExists(path)
              : !ReadRegularFile(path, EditFileBytes(), current, error) ||
                    HashHex(current) != hash)
        return {{"error", "changed since"}};
      if (!JsonValue(request, "existed", false)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return ec ? json{{"error", ec.message()}} : json::object();
      }
      std::string before;
      if (!HexDecode(JsonValue(request, "before", ""), before, MiB(2)))
        return {{"error", "Invalid undo bytes."}};
      auto written = ToolAtomicWrite(
          path, before, static_cast<mode_t>(JsonValue(request, "mode", 0644)),
          true);
      return written.Ok() ? json::object() : json{{"error", written.output}};
    }
    if (operation == "sandbox") {
      ApprovalMode mode;
      if (ParseApprovalMode(JsonValue(request, "approval", "ask"), mode))
        SetApprovalMode(mode);
      return {{"inheritance", SandboxInheritance()},
              {"diagnostic", SandboxDiagnosticJson()}};
    }
    if (operation == "open") {
      std::lock_guard lock(mutex_);
      if (!opened_) {
        // Deliberately no settings/environment capture. Only these policy
        // inputs cross the boundary; defaults cover the execution tool limits.
        const std::string preset = JsonValue(request, "sandbox", "default");
        SandboxPreset parsed;
        if (!ParseSandboxPreset(preset, &parsed))
          return {{"error", "Invalid remote sandbox preset."}};
        if (preset != StringSetting(Cfg("UAGENT_SANDBOX")))
          return {{"error", "Execution sandbox changed; reopen the context."}};
        RegisterFileTools(tools_, processes_, CanonicalCwd());
        RegisterExecTools(tools_, processes_, CanonicalCwd(), nullptr);
        RegisterActivityTool(tools_, processes_);
        opened_ = true;
      }
      json tools = json::array();
      for (const Tool& t : tools_)
        tools.push_back({{"name", t.name},
                         {"description", t.description},
                         {"parameters", t.parameters},
                         {"mutating", t.mutating},
                         {"parallel_safe", t.parallel_safe},
                         {"capabilities", t.capabilities},
                         {"timeout_s", t.timeout_s},
                         {"result_chars", t.result_chars},
                         {"category", t.category}});
      return {{"tools", tools}, {"cwd", CanonicalCwd()}};
    }
    const Tool* tool = nullptr;
    {
      std::lock_guard lock(mutex_);
      if (!opened_) return {{"error", "Execution context is not open."}};
      tool = FindTool(tools_, JsonValue(request, "tool", ""));
    }
    if (!tool) return {{"error", "Unknown execution tool."}};
    ApprovalMode mode;
    if (!ParseApprovalMode(JsonValue(request, "approval", "ask"), mode))
      return {{"error", "Invalid approval mode."}};
    SetApprovalMode(mode);
    json args = request.value("args", json::object());
    if (tool->name == "write_file" && args.contains("content_hex")) {
      std::string bytes;
      if (!HexDecode(JsonValue(args, "content_hex", ""), bytes, MiB(8)))
        return {{"error", "Invalid attachment bytes."}};
      args.erase("content_hex");
      args["content"] = std::move(bytes);
    }
    if (operation == "check") {
      json canonical = args;
      CanonicalizeToolArguments(*tool, canonical);
      auto issue = FindToolArgumentIssue(*tool, canonical);
      if (!issue && tool->validate) issue = tool->validate(canonical);
      json result = {
          {"args", canonical},
          {"mutates", ToolMutates(*tool, canonical)},
          {"approval", static_cast<int>(RequiredApproval(*tool, canonical))},
          {"preview",
           tool->approval_preview ? tool->approval_preview(canonical) : ""},
          {"subject", tool->approval_subject ? tool->approval_subject(canonical)
                                             : canonical},
          {"reason",
           tool->mandatory_reason ? tool->mandatory_reason(canonical) : ""}};
      result["scope"] = Scope(*tool, canonical);
      result["subject"] = result["scope"]["subject"];
      if (issue)
        result["issue"] = {{"code", issue->code},
                           {"message", issue->message},
                           {"field", issue->field}};
      return result;
    }
    if (operation != "run") return {{"error", "Invalid execution request."}};
    if (request.contains("scope") && request["scope"] != Scope(*tool, args))
      return EncodeResult(ToolFailure(
          ToolErrorCode::kPermissionDenied,
          "What this call reaches changed after approval; ask again."));
    ToolContext context;
    context.call_id = JsonValue(request, "call_id", "");
    context.turn_id = JsonValue(request, "turn_id", int64_t{0});
    context.approved = request.value("approved", json());
    context.timeout_s = JsonValue(request, "timeout_s", int64_t{300});
    if (context.timeout_s > 0)
      context.deadline = Clock::now() + std::chrono::seconds(context.timeout_s);
    auto issue = FindToolArgumentIssue(*tool, args);
    if (!issue && tool->validate) issue = tool->validate(args);
    ToolResult result =
        issue ? ToolFailure(ToolErrorCode::kInvalidArguments, issue->message)
              : tool->run(args, context);
    json encoded = EncodeResult(result);
    json media = json::array();
    size_t remaining = MiB(8);
    for (const Attachment& attachment :
         Attachments().TakeForCall(context.call_id)) {
      std::string bytes, error;
      if (!ReadRegularFile(attachment.path, remaining, bytes, error)) {
        encoded["output"] = JsonValue(encoded, "output", "") +
                            "\nAttachment unavailable: " + error;
        continue;
      }
      remaining -= bytes.size();
      media.push_back({{"name", attachment.name}, {"data", HexEncode(bytes)}});
    }
    if (!media.empty()) encoded["attachments"] = std::move(media);
    return encoded;
  }

 public:
  void Tick() {
    std::lock_guard lock(terminal_mutex_);
    std::erase_if(viewers_, [](const auto& entry) {
      return Clock::now() - entry.second.touched > std::chrono::seconds(60);
    });
  }

 private:
  struct Viewer {
    std::shared_ptr<TerminalViewer> viewer;
    Clock::time_point touched;
  };
  std::mutex terminal_mutex_;
  std::unique_ptr<Terminals> terminals_;
  std::map<std::string, Viewer> viewers_;
  ActivitySupervisor processes_;
  std::vector<Tool> tools_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<CallState>> calls_;
  bool opened_ = false;
};
int Serve(const std::string& directory, const std::string& cwd,
          const std::string& auth) {
  (void)setsid();
  SetGracefulShutdown(true);
  if (chdir(cwd.c_str()) != 0) return 66;
  std::string error;
  FileLease lease;
  if (!lease.Acquire(directory + "/owner", error, true)) return 65;
  SetExecutionStateDirectory(directory + "/state");
  if (!EnsurePrivateDirectory(GlobalBase())) return 67;
  const std::string path = directory + "/socket";
  Fd listener = ListenUnix(path, 16);
  if (!listener) return 67;
  auto worker = std::make_unique<Worker>();
  struct Connection {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Connection> connections;
  auto touched = Clock::now();
  while (!ShutdownRequested()) {
    worker->Tick();
    for (auto it = connections.begin(); it != connections.end();) {
      if (*it->done) {
        it->thread.join();
        it = connections.erase(it);
      } else
        ++it;
    }
    pollfd ready{listener.Get(), POLLIN, 0};
    if (poll(&ready, 1, 1000) > 0 && (ready.revents & POLLIN)) {
      Fd fd(accept(listener.Get(), nullptr, nullptr));
      if (!fd || connections.size() >= 64) continue;
      touched = Clock::now();
      auto done = std::make_shared<std::atomic<bool>>(false);
      connections.push_back(
          {std::thread([fd = std::move(fd), &worker, done, &auth]() {
             const json request =
                 Receive(fd.Get(), Clock::now() + std::chrono::seconds(30));
             if (request.is_object())
               Send(fd.Get(),
                    JsonValue(request, "auth", "") == auth
                        ? worker->Request(request)
                        : json{{"error", "Gateway authentication failed."}});
             *done = true;
           }),
           done});
    }
    // A connected main pings every minute. A lost main leaves a finite grace
    // period; no detached process or execution log becomes durable history.
    if (connections.empty() &&
        Clock::now() - touched > std::chrono::minutes(5) &&
        !worker->HasChildren())
      break;
  }
  RequestAbort();
  worker->CancelAll();
  for (auto& c : connections) c.thread.join();
  worker.reset();
  unlink(path.c_str());
  std::error_code ignored;
  std::filesystem::remove_all(GlobalBase(), ignored);
  return 0;
}
}  // namespace
int Main(int argc, char** argv) {
  if (argc != 5 || !session::OpaqueId(argv[2]) || !session::OpaqueId(argv[3]))
    return 64;
  std::string startup;
  if (!std::getline(std::cin, startup) || startup.size() > KiB(4)) return 64;
  const json options = json::parse(startup, nullptr, false);
  const std::string auth = JsonValue(options, "auth", "");
  const std::string preset = JsonValue(options, "sandbox", "");
  if (!session::OpaqueId(auth) || preset.empty()) return 64;
  // Remote account configuration must not affect execution policy. This runs
  // before threads and before any settings capture; HOME/PATH remain remote.
  std::vector<std::string> remove;
  for (char** entry = ProcessEnvironment(); entry && *entry; ++entry) {
    std::string_view value(*entry);
    if (value.starts_with("UAGENT_"))
      remove.emplace_back(value.substr(0, value.find('=')));
  }
  for (const auto& key : remove) unsetenv(key.c_str());
  PublishSettings(json{{"UAGENT_SANDBOX", preset}});
  std::string requested = argv[4];
  if (requested.empty() || requested == "~")
    requested = UserHome();
  else if (requested.starts_with("~/"))
    requested = UserHome() + requested.substr(1);
  const auto folder = CanonicalDirectory(requested);
  if (!folder) {
    printf("%s\n",
           JsonDump(json{{"error", "Choose an accessible remote directory."}})
               .c_str());
    return 66;
  }
  const std::string directory =
      RuntimeDir() + "/exec/" +
      HashHex(std::string(BuildIdentity()) + argv[2] + argv[3] + preset);
  if (!EnsurePrivateDirectory(directory)) return 67;
  const std::string path = directory + "/socket";
  if (std::string_view(argv[1]) == "--execution-worker")
    return Serve(directory, folder->string(), auth);
  auto ping = [&]() {
    Fd fd = ConnectUnix(path);
    return fd && Send(fd.Get(), json{{"op", "ping"}, {"auth", auth}})
               ? Receive(fd.Get(), Clock::now() + std::chrono::seconds(2))
               : json();
  };
  json identity = ping();
  if (identity.is_null()) {
    int descriptors[2];
    if (!OpenNonblockingPipe(descriptors)) return 68;
    Fd source(descriptors[0]), sink(descriptors[1]);
    fcntl(source.Get(), F_SETFL, fcntl(source.Get(), F_GETFL) & ~O_NONBLOCK);
    pid_t pid = -1;
    if (SpawnProcess({.argv = {argv[0], "--execution-worker", argv[2], argv[3],
                               folder->string()},
                      .group = SpawnSpec::Group::kSession,
                      .fds = {{0, source.Get()}, {1}, {2}}},
                     pid))
      return 68;
    source.Reset();
    if (!WriteAllWithin(sink.Get(), startup + "\n", 2000)) return 68;
    sink.Reset();
    for (int i = 0; i < 100 && identity.is_null(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      identity = ping();
    }
  }
  if (identity.is_null()) return 68;
  if (identity.contains("error")) return 68;
  if (JsonValue(identity, "cwd", "") != folder->string()) return 64;
  identity["socket"] = path;
  printf("%s\n", JsonDump(identity).c_str());
  return 0;
}
}  // namespace uagent::execution
