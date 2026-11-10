// Copyright 2026 Timon Gentzsch
#include <unistd.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/activity_supervisor.h"
#include "include/agent/session_store.h"
#include "include/app/asset_store.h"
#include "include/app/execution.h"
#include "include/core/build.h"
#include "include/core/capture.h"
#include "include/core/child_env.h"
#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"
#include "include/media/attachments.h"
#include "include/tools/registry.h"
#include "src/app/execution_internal.h"

namespace uagent::execution {
namespace {
using Clock = std::chrono::steady_clock;
std::shared_ptr<Client> current;
json Unavailable() {
  return {{"error",
           "Remote host is unavailable. Connect or Reconnect in Hosts; "
           "uncertain mutations were not repeated."}};
}
}  // namespace
struct Client::Impl {
  Target target;
  std::string socket;
  std::mutex mutex;
  json opened;
  std::string completion_request, error;
  explicit Impl(Target value) : target(std::move(value)) {
    if (target.sandbox.empty()) {
      target.sandbox = StringSetting(Cfg("UAGENT_SANDBOX"));
    }
    socket = RuntimeDir() + "/e-" +
             HashHex(target.client + target.runtime + target.sandbox) + ".sock";
  }
  std::vector<std::string> Ssh() const {
    return {"ssh",
            "-S",
            target.control,
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "ForwardAgent=no",
            "-o",
            "SendEnv=-*",
            "-o",
            "ConnectTimeout=10",
            "-o",
            "ControlMaster=no",
            target.address};
  }
  json Exchange(const json& request, const ToolContext* context = nullptr) {
    Fd fd = ConnectUnix(socket);
    json authenticated = request;
    authenticated["auth"] = target.auth;
    if (!fd || !Send(fd.Get(), authenticated)) return json();
    return Receive(
        fd.Get(),
        context
            ? context->deadline
            : Clock::now() +
                  std::chrono::seconds(
                      JsonValue(request, "op", "") == "activities" ? 2 : 30),
        context != nullptr);
  }
  CapturedProcess SshRun(const std::vector<std::string>& args, int seconds,
                         const std::string& input = "") {
    ChildEnvironment environment({}, ChildEnvironmentPolicy::kUserShell);
    return CaptureProcess(args, seconds, input, environment.Data());
  }
  json Failure() {
    std::lock_guard lock(mutex);
    return error.empty() ? Unavailable() : json{{"error", error}};
  }
  bool Attach() {
    std::lock_guard lock(mutex);
    if (!Exchange({{"op", "ping"}}).is_null()) return true;
    // Files, terminals and the conversation process share this forward. A
    // transport loss must have one reattacher across all main processes.
    FileLease lease;
    std::string lease_error;
    error.clear();
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (!lease.Acquire(socket + ".lock", lease_error)) {
      if (Clock::now() >= until) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!Exchange({{"op", "ping"}}).is_null()) return true;
    // Never bypass the main's explicit Disconnect with a fresh SSH session.
    auto check = Ssh();
    check.insert(check.begin() + 1, {"-O", "check"});
    if (!SshRun(check, 5).Ok()) return false;
    auto launch = Ssh();
    launch.push_back(target.executable + " --execution-start " +
                     ShellQuote(target.client) + " " +
                     ShellQuote(target.runtime) + " " + ShellQuote(target.cwd));
    const auto process = SshRun(
        launch, 15,
        JsonDump({{"auth", target.auth}, {"sandbox", target.sandbox}}) + "\n");
    if (!process.Ok()) {
      error =
          JsonValue(json::parse(process.output, nullptr, false), "error", "");
      return false;
    }
    const json identity = json::parse(process.output, nullptr, false);
    const std::string remote = JsonValue(identity, "socket", "");
    if (JsonValue(identity, "build", "") != BuildIdentity() || remote.empty() ||
        remote.front() != '/' ||
        remote.find_first_of(":\n\r") != std::string::npos) {
      return false;
    }
    auto cancel = Ssh();
    cancel.insert(cancel.begin() + 1,
                  {"-O", "cancel", "-L", socket + ":" + remote});
    (void)SshRun(cancel, 5);
    unlink(socket.c_str());
    auto forward = Ssh();
    forward.insert(forward.begin() + 1,
                   {"-O", "forward", "-L", socket + ":" + remote});
    return SshRun(forward, 10).Ok() && !Exchange({{"op", "ping"}}).is_null();
  }
};
Target TargetFromJson(const json& v) {
  return {JsonValue(v, "address", ""),    JsonValue(v, "control", ""),
          JsonValue(v, "executable", ""), JsonValue(v, "client", ""),
          JsonValue(v, "runtime", ""),    JsonValue(v, "cwd", ""),
          JsonValue(v, "auth", ""),       JsonValue(v, "sandbox", "")};
}
Client::Client(Target target)
    : impl_(std::make_unique<Impl>(std::move(target))) {}
Client::~Client() = default;
void SetClient(std::shared_ptr<Client> value) {
  current = std::move(value);
  SetWorkspaceRequest([](const json& request) {
    return current ? current->Call(request) : Unavailable();
  });
}
std::shared_ptr<Client> CurrentClient() { return current; }
json Client::Call(json request, const ToolContext* context) {
  const std::string op = JsonValue(request, "op", "");
  if ((op == "run" || op == "check") && request.contains("args")) {
    const std::string args = JsonDump(request["args"]);
    const std::string attachments = UagentDir(kHistoryDir) + "/";
    if (args.find(attachments) != std::string::npos) {
      return {{"error",
               "Attachments live on the main host. Copy one with write_file "
               "from= before using it remotely."}};
    }
  }
  request["approval"] = ApprovalModeName(CurrentApprovalMode());
  const bool completion = op == "completions";
  if (completion) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->completion_request.empty()) {
      impl_->completion_request = RandomToken(16);
    }
    request["id"] = impl_->completion_request;
  }
  const bool run =
      op == "run" || op == "schedule_check" || completion || op == "undo" ||
      op == "activity_control" || op == "terminal" ||
      (op == "files" && JsonValue(request, "mode", "") == "write") ||
      (op == "instructions" && JsonValue(request, "action", "") == "write");
  if (run) {
    if (!request.contains("id")) request["id"] = RandomToken(16);
    if (context) {
      request["call_id"] = context->call_id;
      request["turn_id"] = context->turn_id;
      request["approved"] = context->approved;
      request["timeout_s"] = context->RemainingSeconds(300);
    }
  }
  json result;
  // A new viewer is safe to attach only after its forward is live. A stale
  // socket left by a disconnected SSH master cannot have accepted this attach.
  if (op == "terminal" && JsonValue(request, "action", "") == "attach" &&
      !impl_->Attach()) {
    return impl_->Failure();
  }
  {
    if (access(impl_->socket.c_str(), F_OK) != 0 &&
        (op == "activities" || !impl_->Attach())) {
      return impl_->Failure();
    }
  }
  result = impl_->Exchange(request, context);
  auto received = [&](json value) {
    if (completion) {
      std::lock_guard lock(impl_->mutex);
      impl_->completion_request.clear();
    }
    return value;
  };
  if (!result.is_null() && !JsonValue(result, "pending", false)) {
    return received(std::move(result));
  }
  if (!run) {
    if (op == "activities") return Unavailable();
    if (!impl_->Attach()) return Unavailable();
    const json retried = impl_->Exchange(request, context);
    return retried.is_null() ? Unavailable() : retried;
  }
  // The command might already have changed a file or started a process. Only
  // query its receipt after a drop; executing it again is never recovery.
  if (!impl_->Attach()) return Unavailable();
  const std::string id = JsonValue(request, "id", "");
  if (AbortRequested()) impl_->Exchange({{"op", "cancel"}, {"id", id}});
  const auto deadline =
      context ? context->deadline : Clock::now() + std::chrono::seconds(30);
  while (Clock::now() < deadline) {
    json receipt = impl_->Exchange({{"op", "result"}, {"id", id}});
    if (AbortRequested()) impl_->Exchange({{"op", "cancel"}, {"id", id}});
    if (!JsonValue(receipt, "known", false)) return received(Unavailable());
    if (JsonValue(receipt, "done", false)) {
      return received(receipt.value("result", json()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return Unavailable();
}
bool Client::Open(std::string& error) {
  impl_->opened = Call({{"op", "open"}, {"sandbox", impl_->target.sandbox}});
  error = JsonValue(impl_->opened, "error", "");
  return error.empty() && JsonArray(impl_->opened, "tools");
}
Target Client::ChildTarget(const std::string& runtime) const {
  Target target = impl_->target;
  target.runtime = runtime;
  return target;
}
std::vector<Tool> Client::Tools(const std::string& session_path) {
  // Only pure presentation hints are kept from the built-in definitions. Every
  // filesystem-dependent hook is replaced, including validation and approval.
  static ActivitySupervisor unused;
  std::vector<Tool> tools = BuiltinTools(unused, impl_->target.cwd, nullptr);
  const json* descriptions = JsonArray(impl_->opened, "tools");
  std::vector<Tool> forwarded;
  if (!descriptions) return forwarded;
  for (const json& description : *descriptions) {
    const std::string name = JsonValue(description, "name", "");
    const Tool* original = FindTool(tools, name);
    if (!original) continue;
    Tool tool = *original;
    tool.description = JsonValue(description, "description", "");
    tool.parameters = description.value("parameters", json::object());
    tool.mutating = JsonValue(description, "mutating", false);
    tool.parallel_safe = JsonValue(description, "parallel_safe", false);
    tool.timeout_s = JsonValue(description, "timeout_s", tool.timeout_s);
    tool.result_chars =
        JsonValue(description, "result_chars", tool.result_chars);
    tool.capabilities = description.value("capabilities", tool.capabilities);
    tool.category = JsonValue(description, "category", tool.category);
    struct CheckCache {
      std::mutex mutex;
      json args, result;
      ApprovalMode mode = ApprovalMode::kAsk;
      Clock::time_point at;
    };
    auto cache = std::make_shared<CheckCache>();
    auto check = [this, name, cache](const json& args) {
      std::lock_guard lock(cache->mutex);
      const auto mode = CurrentApprovalMode();
      if (cache->args == args && cache->mode == mode &&
          Clock::now() - cache->at < std::chrono::seconds(1) &&
          !cache->result.is_null()) {
        return cache->result;
      }
      const json result =
          Call({{"op", "check"}, {"tool", name}, {"args", args}});
      if (!result.contains("error")) {
        cache->args = args;
        cache->mode = mode;
        cache->result = result;
        cache->at = Clock::now();
      }
      return result;
    };
    tool.canonicalize = [check](json& args) {
      const json value = check(args);
      if (const json* canonical = JsonObject(value, "args")) args = *canonical;
    };
    tool.validate =
        [check](const json& args) -> std::optional<ToolArgumentIssue> {
      json value = check(args);
      if (const std::string failure = JsonValue(value, "error", "");
          !failure.empty()) {
        return ArgumentIssue("remote.unavailable", failure);
      }
      if (const json* issue = JsonObject(value, "issue")) {
        return ArgumentIssue(JsonValue(*issue, "code", "remote.invalid"),
                             JsonValue(*issue, "message", "Invalid arguments"),
                             JsonValue(*issue, "field", ""));
      }
      return std::nullopt;
    };
    tool.mutates = [check](const json& args) {
      return JsonValue(check(args), "mutates", true);
    };
    tool.needs_approval = [check](const json& args) {
      const json value = check(args);
      return JsonValue(value.value("scope", json::object()), "outside", true);
    };
    tool.approval_class = [check](const json& args) {
      return JsonValue(check(args), "approval", 2) == 2
                 ? ApprovalClass::kMandatoryHuman
                 : ApprovalClass::kNone;
    };
    tool.approval_preview = [check](const json& args) {
      return JsonValue(check(args), "preview", "");
    };
    tool.approval_subject = [check](const json& args) {
      return check(args).value("subject", args);
    };
    tool.mandatory_reason = [check](const json& args) {
      return JsonValue(check(args), "reason", "");
    };
    tool.run = [this, name, cache, session_path](const json& args,
                                                 const ToolContext& context) {
      {
        std::lock_guard lock(cache->mutex);
        cache->result = json();
      }
      const json checked =
          Call({{"op", "check"}, {"tool", name}, {"args", args}});
      if (checked.contains("error")) return DecodeResult(checked);
      const json scope = checked.value("scope", json());
      if (!context.approved.is_null() &&
          context.approved != scope.value("subject", json())) {
        return ToolFailure(
            ToolErrorCode::kPermissionDenied,
            "What this call reaches changed after approval; ask again.");
      }
      const json response = Call(
          {{"op", "run"}, {"tool", name}, {"args", args}, {"scope", scope}},
          &context);
      ToolResult result = DecodeResult(response);
      if (const json* media = JsonArray(response, "attachments")) {
        for (const json& item : *media) {
          std::string bytes;
          if (session_path.empty() ||
              !HexDecode(JsonValue(item, "data", ""), bytes,
                         session::kUploadBytes)) {
            result.output +=
                "\nCannot retain remote attachment for this conversation.";
            continue;
          }
          auto stored = session::SessionAssets().Store(
              session_path, bytes, JsonValue(item, "name", "attachment"), true,
              true);
          if (!stored.error.empty()) {
            result.output +=
                "\nCannot retain remote attachment: " + stored.error;
            continue;
          }
          Attachment attachment = AttachmentFromJson(stored.value);
          attachment.bytes = bytes.size();
          const std::string stem =
              session_path + kSessionAssetsSuffix + "/" + attachment.asset_id;
          attachment.path = stem + session::ReadAssetRecord(stem).extension;
          attachment.source_call_id = context.call_id;
          const ToolResult queued = Attachments().Add(std::move(attachment));
          if (!queued.Ok()) result.output += "\n" + queued.output;
        }
      }
      return result;
    };
    forwarded.push_back(std::move(tool));
  }
  return forwarded;
}
}  // namespace uagent::execution
