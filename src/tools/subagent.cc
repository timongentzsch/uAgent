// Copyright 2026 Timon Gentzsch

#include "include/tools/subagent.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/tools/collaborator_runtime.h"
#include "include/tools/files.h"
#include "include/tools/shell.h"

namespace uagent {
namespace {

// Enough to show the shape of a configured roster without charging the whole
// list to every request; uagent action=inspect topic=routes reports all of
// them.
constexpr size_t kAdvertisedRoutes = 4;
constexpr int kCollaboratorFormat = 1;
constexpr size_t kAgentNameMax = 32;
constexpr size_t kAgentDescriptionMax = 280;
constexpr int kMailMaxHops = 8;

std::string CollaboratorPath(const std::string& id) {
  return UagentDir("collaborators") + "/" + id + ".json";
}

std::string CollaboratorSessionPath(const std::string& id) {
  return UagentDir("collaborators") + "/" + id + ".session.json";
}

std::string CollaboratorCommunicationPath(const std::string& id) {
  return UagentDir("collaborators") + "/" + id + ".comms.jsonl";
}

json CollaboratorCommunication(const std::string& id) {
  constexpr int64_t kTailBytes = int64_t{64} * 1024;
  int64_t start = 0;
  std::istringstream input(
      ReadFileTail(CollaboratorCommunicationPath(id), kTailBytes, &start));
  if (start > 0) {  // the tail began mid-line
    std::string partial;
    std::getline(input, partial);
  }
  json messages = json::array();
  std::string line;
  while (std::getline(input, line)) {
    json event = json::parse(line, nullptr, false);
    if (!event.is_object()) continue;
    messages.push_back(std::move(event));
    if (messages.size() > kMaxCollaboratorRecords) {
      messages.erase(messages.begin());
    }
  }
  return messages;
}

// Short because the id is quoted back in every spawn, followup, message and
// list result -- a recurring token cost for something no human types. The
// digest is the same FNV-1a construction session ids use, truncated to eight
// hex digits and retried against the files it would name. Retrying is not
// exclusive creation: nothing is created here, so two processes can still
// agree on a free id at the same instant. That residual is accepted -- the
// inputs already include the pid, and the alternative is an O_EXCL placeholder
// on a path the caller may never write.
std::string NewCollaboratorId() {
  static std::atomic<uint64_t> sequence{0};
  const std::string seed =
      CanonicalCwd() + ":" + std::to_string(getpid()) + ":" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      ":" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  std::error_code code;
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::string id =
        "agent-" +
        TruncatedHash(seed + ":" + std::to_string(attempt), kAgentNameChars);
    if (!std::filesystem::exists(CollaboratorPath(id), code) &&
        !std::filesystem::exists(CollaboratorSessionPath(id), code)) {
      return id;
    }
  }
  // Eight collisions in a row is a broken digest, not bad luck. Fall back to
  // the full width rather than handing back an id that is known to be taken.
  return "agent-" + HashHex(seed);
}

// Mail is consumed before the launch so a child already draining it is not
// handed the same guidance twice -- which leaves every path that gives up
// between here and the launch holding messages nobody asked for. Writing them
// back on the way out is the only place that covers all of them.
struct MailRestore {
  std::string id;
  std::vector<QueuedMessage> taken;

  ~MailRestore() {
    for (const QueuedMessage& queued : taken) {
      WriteCollaboratorMail(id, queued.text, queued.from, queued.hops);
    }
  }
};

bool ValidAgentName(std::string_view name) {
  if (name.empty() || name.size() > kAgentNameMax) return false;
  if (name.front() == '-' || name.back() == '-') return false;
  for (char c : name) {
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (!lower && !digit && c != '-') return false;
  }
  return true;
}

// Same-team peers see each other's records; other sessions stay isolated.
// The owner check preserves the old boundary, the team check opens the swarm.
bool CollaboratorAllowed(const ProcessSupervisor& processes,
                         const json& state) {
  if (JsonValue(state, "owner", "") == processes.Owner()) return true;
  const std::string team = JsonValue(state, "team", "");
  const std::string mine = OwnTeam();
  return !team.empty() && !mine.empty() && team == mine;
}

bool LoadCollaborator(const std::string& id, json& state, std::string& error) {
  if (id.empty() || SafeFileComponent(id) != id) {
    error = "invalid collaborator id";
    return false;
  }
  std::ifstream input(CollaboratorPath(id));
  if (!input) {
    error = "collaborator not found";
    return false;
  }
  state = json::parse(input, nullptr, false);
  if (state.is_discarded() || !state.is_object() ||
      JsonValue(state, "format", 0) != kCollaboratorFormat ||
      JsonValue(state, "id", "") != id) {
    error = "collaborator record is invalid";
    return false;
  }
  if (JsonValue(state, "cwd", "") != CanonicalCwd()) {
    error = "collaborator belongs to a different workspace";
    return false;
  }
  return true;
}

ToolResult SaveCollaborator(const json& state) {
  return ToolAtomicWrite(CollaboratorPath(JsonValue(state, "id", "")),
                         JsonDump(state, 2) + "\n", kPrivateFileMode,
                         /*preserve_mode=*/true);
}

std::optional<int64_t> ActiveCollaborator(const ProcessSupervisor& processes,
                                          const std::string& id) {
  for (const BgJob& job : processes.Snapshot()) {
    if (job.source_id == id) return ActivityId(job);
  }
  return std::nullopt;
}

}  // namespace

std::vector<json> CollaboratorSummaries(const ProcessSupervisor& processes,
                                        const CollaboratorRuntime* runtime) {
  namespace fs = std::filesystem;
  std::error_code error;
  std::vector<json> records;
  for (fs::directory_iterator it(UagentDir("collaborators"), error), end;
       !error && it != end && records.size() < kMaxCollaboratorRecords;
       it.increment(error)) {
    const std::string name = it->path().filename().string();
    // Undelivered mail is a sibling file, not a collaborator: skipping it by
    // name keeps a talkative parent from crowding out the records below.
    if (!it->is_regular_file(error) || !name.ends_with(".json") ||
        name.ends_with(".session.json") ||
        name.find(".mail-") != std::string::npos ||
        name.ends_with(".comms.jsonl")) {
      continue;
    }
    std::ifstream input(it->path());
    json state = json::parse(input, nullptr, false);
    if (state.is_discarded() || !state.is_object() ||
        JsonValue(state, "cwd", "") != CanonicalCwd()) {
      continue;
    }
    const std::string team = JsonValue(state, "team", "");
    if (!processes.Owner().empty() &&
        JsonValue(state, "owner", "") != processes.Owner() &&
        (team.empty() || team != OwnTeam() || OwnTeam().empty())) {
      continue;
    }
    std::string id = JsonValue(state, "id", "");
    std::optional<int64_t> activity = ActiveCollaborator(processes, id);
    json row = {{"id", id},
                {"name", JsonValue(state, "name", "")},
                {"description", JsonValue(state, "description", "")},
                {"team", team},
                {"model", JsonValue(state, "model", "")},
                {"label", JsonValue(state, "label", "Subagent")},
                {"mode", JsonValue(state, "mode", "lean")},
                {"persistent", JsonValue(state, "persistent", false)},
                {"status", activity ? "running" : "idle"}};
    if (runtime) {
      json retained = runtime->Snapshot(id);
      if (!retained.empty()) row.update(retained);
    }
    // The handle the activity tool wants, so a caller that sees "running" does
    // not have to guess at one to wait on or stop it.
    if (activity) row["activity"] = *activity;
    records.push_back(std::move(row));
  }
  return records;
}

ToolResult MessageCollaborator(const ProcessSupervisor& processes,
                               CollaboratorRuntime* runtime,
                               const std::string& id, const std::string& text,
                               const std::string& from, int hops) {
  json record;
  std::string error;
  if (!LoadCollaborator(id, record, error)) {
    return ToolFailure(ToolErrorCode::kNotFound, error);
  }
  if (!CollaboratorAllowed(processes, record)) {
    return ToolFailure(ToolErrorCode::kPermissionDenied,
                       "collaborator belongs to another conversation");
  }
  if (text.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "message requires text");
  }
  if (hops >= kMailMaxHops) {
    return ToolSuccess("dropped message for collaborator " + id +
                       " [loop-clamped]");
  }
  const std::string sender =
      from.empty()
          ? (OwnCollaboratorId().empty() ? "parent" : OwnCollaboratorId())
          : from;
  // Mail-first: the file is the delivery. The socket is only a wake-up ping
  // for a live persistent child; its result is best-effort and never the
  // receipt, so a crash between accept and drain repeats instead of losing.
  ToolResult saved = WriteCollaboratorMail(id, text, sender, hops);
  if (!saved.Ok()) return saved;
  const std::string event = JsonDump({{"from", sender},
                                      {"to", id},
                                      {"text", Utf8Trunc(text, 4096)},
                                      {"time", UtcStamp()}});
  std::string journal_error;
  (void)AppendPrivateLine(CollaboratorCommunicationPath(id), event,
                          journal_error);
  if (sender != "parent" && sender != id) {
    (void)AppendPrivateLine(CollaboratorCommunicationPath(sender), event,
                            journal_error);
  }
  if (runtime && JsonValue(record, "persistent", false)) {
    (void)runtime->Message(id, "");
  }
  return ToolSuccess("queued message for collaborator " + id);
}

json InspectCollaborator(const ProcessSupervisor& processes,
                         const std::string& id, const json& request,
                         const CollaboratorRuntime* runtime) {
  json state;
  std::string error;
  if (!LoadCollaborator(id, state, error)) return {{"error", error}};
  if (!CollaboratorAllowed(processes, state)) {
    return {{"error", "collaborator belongs to another conversation"}};
  }
  auto loaded = SessionStore::Inspect(CollaboratorSessionPath(id));
  json detail = {{"agent_id", id},
                 {"name", JsonValue(state, "name", "")},
                 {"description", JsonValue(state, "description", "")},
                 {"team", JsonValue(state, "team", "")},
                 {"label", JsonValue(state, "label", "Subagent")},
                 {"task", JsonValue(state, "task", "")},
                 {"directive", JsonValue(state, "directive", "")},
                 {"persistent", JsonValue(state, "persistent", false)},
                 {"route", JsonValue(state, "route", "")},
                 {"communication", CollaboratorCommunication(id)}};
  if (runtime) {
    json retained = runtime->Snapshot(id);
    if (!retained.empty()) detail.update(retained);
  }
  const json live_state = runtime ? runtime->LiveState(id) : json::object();
  json live_view = JsonValue(live_state, "view", json::object());
  const auto apply_live_state = [&] {
    detail["statistics_live"] =
        live_state.contains("statistics") && live_state.contains("usage");
    for (const char* field :
         {"usage", "statistics", "turns", "route", "context_tokens",
          "context_window", "phase", "activity_detail"}) {
      if (live_state.contains(field)) detail[field] = live_state[field];
    }
    if (live_state.contains("activity")) {
      detail["progress"] = live_state["activity"];
    }
  };
  if (!loaded.record) {
    if (!live_view.empty()) detail["conversation"] = std::move(live_view);
    apply_live_state();
    return detail;
  }
  const auto& record = *loaded.record;
  Conversation conversation;
  if (!conversation.Restore(record.state.messages, record.state.message_kinds,
                            record.state.archive,
                            record.state.archive_dropped_segments,
                            record.state.tool_displays, record.state.display)) {
    return {{"error", "invalid child conversation"}};
  }
  const std::string message = JsonValue(request, "detail", "");
  if (!message.empty()) {
    const size_t offset = JsonValue(request, "offset", size_t{0});
    json body = JsonValue(request, "raw", false)
                    ? ConversationExchange(conversation, message, offset)
                    : ConversationDetail(conversation, message, offset);
    return body.contains("error") ? body : json{{"body", std::move(body)}};
  }
  detail.update(
      {{"conversation",
        !live_view.empty() && !JsonValue(request, "before", uint64_t{0})
            ? std::move(live_view)
            : ConversationView(conversation,
                               JsonValue(request, "before", uint64_t{0}))},
       {"turns", record.metadata.turns},
       {"model", record.metadata.model},
       {"context_tokens", record.state.context_tokens},
       {"context_window", record.state.context_window},
       {"statistics", conversation.Statistics()},
       {"usage", UsageJson(record.state.usage)}});
  if (!record.state.last_sent_prompt.empty()) {
    detail["system_prompt"] = record.state.last_sent_prompt;
  }
  apply_live_state();
  return detail;
}

namespace {

std::string JoinSelections(std::vector<std::string> selections) {
  std::sort(selections.begin(), selections.end());
  selections.erase(std::unique(selections.begin(), selections.end()),
                   selections.end());
  size_t total = selections.size();
  if (selections.size() > kAdvertisedRoutes) {
    selections.resize(kAdvertisedRoutes);
  }
  std::string result;
  for (const std::string& selection : selections) {
    if (!result.empty()) result += ", ";
    result += selection;
  }
  if (total > selections.size()) {
    result += ", +" + std::to_string(total - selections.size()) + " more";
  }
  return result;
}

// The per-child ceilings live under one `limits` object. An absent object and
// an absent field mean the same thing -- inherit the configured default -- so
// every reader goes through here rather than testing for the object first.
const json& ChildLimits(const json& arguments) {
  static const json kNone = json::object();
  const json* limits = JsonObject(arguments, "limits");
  return limits != nullptr ? *limits : kNone;
}

std::string ModelPropertyDescription(
    const std::vector<ModelRoute>& routes,
    const std::vector<NamedProvider>& providers) {
  std::vector<std::string> aliases;
  aliases.reserve(routes.size());
  for (const ModelRoute& route : routes) aliases.push_back(route.name);

  // Naming an alias here is an override, not the default. Spelling that out
  // matters: a child sent to an unreachable alias fails outright rather than
  // falling back, so the default must read as the safe choice.
  std::string description =
      "Child model route. Omit to inherit the delegated default in runtime "
      "context; name one only to override it for this subtask.";
  std::string configured = JoinSelections(std::move(aliases));
  if (!configured.empty()) {
    description += " Overrides: " + configured + ".";
  }
  // The grammar, not the enumeration. Naming every provider here charged a
  // list to every request; withholding the grammar entirely is what produced
  // guessed selections. The shape stays; uagent inspection reports the roster.
  if (!providers.empty()) {
    description +=
        " Or <provider>/MODEL for a configured provider; uagent "
        "action=inspect topic=routes lists them.";
  }
  return description;
}

// A delegated child runs on the parent's route unless the request or
// UAGENT_SUBAGENT_MODEL names one; an empty selection tells the shared
// resolver to inherit.
SideRoute ResolveSubagentRoute(const Api& api,
                               const std::vector<ModelRoute>& routes,
                               const std::vector<NamedProvider>& providers,
                               const std::string& requested) {
  return ResolveSideRoute(
      api, routes, providers,
      requested.empty() ? NormalizeModelId(SubagentModel()) : requested);
}

std::string SubagentTargetLabel(const Api& api,
                                const std::vector<ModelRoute>& routes,
                                const std::vector<NamedProvider>& providers,
                                const std::string& requested) {
  return RouteSelection(ResolveSubagentRoute(api, routes, providers, requested),
                        providers);
}

std::string SubagentDiagnosticRoute(
    const SideRoute& route, const std::vector<NamedProvider>& providers) {
  std::string selected = route.selection;
  std::string resolved = RouteSelection(route, providers);
  std::string label = selected.empty() ? resolved : selected;
  if (!resolved.empty() && resolved != label) label += " -> " + resolved;
  std::string host = UrlHost(route.base_url);
  if (!host.empty()) label += " @ " + host;
  return label;
}

}  // namespace

Tool SubagentTool(const Api& api, ProcessSupervisor& processes,
                  const std::vector<ModelRoute>& routes,
                  const std::vector<NamedProvider>& providers, bool debug,
                  CollaboratorRuntime* runtime) {
  json properties = {
      {"operation",
       {{"type", "string"},
        {"enum", json::array({"spawn", "followup", "message", "list"})},
        {"description",
         "spawn default; followup resumes a durable child; message "
         "delivers guidance to a running child between its steps and "
         "otherwise holds it for the next followup; list shows "
         "collaborators"}}},
      {"agent_id",
       {{"type", json::array({"string", "array"})},
        {"items", {{"type", "string"}}},
        {"description",
         "durable collaborator id for followup or message; message "
         "accepts an array for fan-out"}}},
      {"name",
       {{"type", "string"},
        {"maxLength", 32},
        {"description",
         "spawn/followup: reusable role like api-reviewer, lowercase "
         "letters/digits/hyphens; identify expertise for reuse"}}},
      {"description",
       {{"type", "string"},
        {"maxLength", 280},
        {"description",
         "spawn/followup: 1-2 sentences on expertise and when to reuse"}}},
      {"broadcast",
       {{"type", "boolean"},
        {"description",
         "message only: fan out to every teammate in this conversation"}}},
      {"hops",
       {{"type", "integer"},
        {"minimum", 0},
        {"maximum", 8},
        {"description", "message only: peer-forward count for loop clamping"}}},
      {"prompt",
       {{"type", "string"},
        {"description",
         "standalone brief for spawn; next message for a "
         "followup or queued message"}}},
      {"directive",
       {{"type", "string"},
        {"description",
         "persistent coordinator guidance prepended to followups; an "
         "explicit empty string clears it"}}},
      {"background",
       {{"type", "boolean"},
        {"description",
         "default true for ordinary children and false for persistent ones; "
         "false blocks and returns the handoff result directly"}}},
      {"persistent",
       {{"type", "boolean"},
        {"description",
         "spawn only; retain one live sidekick runtime for blocking "
         "followups"}}},
      {"mode",
       {{"type", "string"},
        {"enum", json::array({"lean", "full"})},
        {"description", "lean default; full includes implementation tools"}}},
      {"model",
       {{"type", "string"},
        {"description", ModelPropertyDescription(routes, providers)}}},
      // One object rather than five siblings: each of these needed a sentence
      // saying "optional ceiling for this child", and that sentence is charged
      // to every request that advertises the tool. Grouped, it is said once.
      {"limits",
       {{"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
          {"tool_calls",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
          {"seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
          {"cost", {{"type", "number"}, {"minimum", 0}}},
          {"memory", {{"type", "boolean"}}}}},
        {"description",
         "optional per-child ceilings; an omitted field inherits the "
         "configured default, memory=false denies the child memory, and a "
         "value above the host ceiling is clamped and reported"}}}};
  Tool tool = MakeTool(
      "subagent",
      "Delegate an isolated subtask whose compact result avoids multiple "
      "parent rounds; for a broad request with orthogonal parts, issue one "
      "task per part in a single batch. Spawn creates a durable collaborator "
      "whose conversation can be resumed with operation=followup; message "
      "queues guidance a child reads at its next step (live children get a "
      "wake-up ping), broadcast fans out to teammates, while activity "
      "handles waiting, output and stopping. Always set name+description at "
      "spawn: name the reusable role, describe expertise and reuse. Keep "
      "background=true when useful parent work can continue.",
      {{"type", "object"}, {"properties", std::move(properties)}},
      [&api, &routes, &providers, debug, &processes, runtime](
          const json& arguments, const ToolContext& context) {
        std::string operation = JsonValue(arguments, "operation", "spawn");
        if (operation == "list") {
          std::vector<json> collaborators =
              CollaboratorSummaries(processes, runtime);
          return ToolSuccess(collaborators.empty()
                                 ? "no collaborators"
                                 : JsonDump(collaborators, 2));
        }
        if (operation != "spawn" && operation != "followup" &&
            operation != "message") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: operation must be spawn, followup, "
                             "message, or list");
        }

        std::string collaborator_id = JsonValue(arguments, "agent_id", "");
        // message fan-out: single id, array of ids, or team broadcast.
        std::vector<std::string> target_ids;
        if (operation == "message") {
          const bool broadcast = JsonValue(arguments, "broadcast", false);
          const json* ids = JsonArray(arguments, "agent_id");
          if (ids != nullptr) {
            for (const json& entry : *ids) {
              if (entry.is_string()) {
                target_ids.push_back(entry.get<std::string>());
              }
            }
          } else if (!collaborator_id.empty()) {
            target_ids.push_back(collaborator_id);
          } else if (broadcast) {
            for (const json& row : CollaboratorSummaries(processes, runtime)) {
              target_ids.push_back(JsonValue(row, "id", ""));
            }
            target_ids.erase(
                std::remove_if(
                    target_ids.begin(), target_ids.end(),
                    [](const std::string& id) { return id.empty(); }),
                target_ids.end());
          }
        }
        json collaborator;
        bool persistent = false;
        if (operation == "spawn") {
          if (!collaborator_id.empty()) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: agent_id is assigned by spawn");
          }
          collaborator_id = NewCollaboratorId();
          const std::string name = JsonValue(arguments, "name", "");
          if (!name.empty() && !ValidAgentName(name)) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: name must match [a-z0-9-]{1,32}, no "
                               "leading/trailing '-'");
          }
          collaborator = {
              {"format", kCollaboratorFormat},
              {"id", collaborator_id},
              {"cwd", CanonicalCwd()},
              {"owner", processes.Owner()},
              {"team", processes.Owner()},
              {"name", name},
              {"description", Utf8Trunc(JsonValue(arguments, "description", ""),
                                        kAgentDescriptionMax)},
              {"session_file", CollaboratorSessionPath(collaborator_id)},
              {"created_at", UtcStamp()},
              {"directive", JsonValue(arguments, "directive", "")},
              {"persistent", JsonValue(arguments, "persistent", false)}};
          persistent = JsonValue(collaborator, "persistent", false);
          if (persistent && runtime &&
              runtime->Count() >= static_cast<size_t>(PersistentMax())) {
            return ToolFailure(
                ToolErrorCode::kLimitExceeded,
                "error: persistent limit reached (" +
                    std::to_string(PersistentMax()) +
                    "); stop a retained collaborator to free its runtime");
          }
        } else {
          if (arguments.contains("persistent")) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: persistent is fixed when spawning");
          }
          std::string load_error;
          if (operation != "message") {
            if (!LoadCollaborator(collaborator_id, collaborator, load_error)) {
              return ToolFailure(ToolErrorCode::kNotFound,
                                 "error: " + load_error);
            }
            if (!CollaboratorAllowed(processes, collaborator)) {
              return ToolFailure(
                  ToolErrorCode::kInvalidArguments,
                  "collaborator belongs to another conversation");
            }
            persistent = JsonValue(collaborator, "persistent", false);
          }
        }

        std::string prompt = JsonValue(arguments, "prompt", "");
        if (operation == "message") {
          if (arguments.contains("directive")) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: message cannot change directive; use "
                               "followup");
          }
          if (target_ids.empty()) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: message requires agent_id or broadcast");
          }
          const int hops =
              static_cast<int>(JsonValue(arguments, "hops", int64_t{0}));
          const std::string from =
              OwnCollaboratorId().empty() ? "parent" : OwnCollaboratorId();
          std::string combined;
          for (const std::string& target : target_ids) {
            ToolResult one = MessageCollaborator(processes, runtime, target,
                                                 prompt, from, hops);
            if (!combined.empty()) combined += "\n";
            combined +=
                one.Ok() ? one.output : ("error " + target + ": " + one.output);
            if (!one.Ok()) return ToolFailure(one.error, combined);
          }
          return ToolSuccess(combined);
        }

        if (runtime && runtime->Active(collaborator_id)) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: collaborator " + collaborator_id +
                                 " already has an active handoff");
        }
        if (std::optional<int64_t> active =
                ActiveCollaborator(processes, collaborator_id)) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: collaborator " + collaborator_id +
                                 " is already running as activity " +
                                 std::to_string(*active));
        }
        if (operation == "followup") {
          if (persistent &&
              (arguments.contains("model") || arguments.contains("mode") ||
               arguments.contains("limits"))) {
            return ToolFailure(
                ToolErrorCode::kInvalidArguments,
                "error: a persistent collaborator retains its model, mode, "
                "and limits");
          }
          if (arguments.contains("directive")) {
            collaborator["directive"] = JsonValue(arguments, "directive", "");
          }
          if (arguments.contains("name")) {
            const std::string name = JsonValue(arguments, "name", "");
            if (!name.empty() && !ValidAgentName(name)) {
              return ToolFailure(ToolErrorCode::kInvalidArguments,
                                 "error: name must match [a-z0-9-]{1,32}, no "
                                 "leading/trailing '-'");
            }
            collaborator["name"] = name;
          }
          if (arguments.contains("description")) {
            collaborator["description"] = Utf8Trunc(
                JsonValue(arguments, "description", ""), kAgentDescriptionMax);
          }
          if (arguments.contains("team") &&
              JsonValue(arguments, "team", "") !=
                  JsonValue(collaborator, "team", "")) {
            return ToolFailure(ToolErrorCode::kInvalidArguments,
                               "error: team is fixed when spawning");
          }
        }
        std::string directive = JsonValue(collaborator, "directive", "");
        if (!directive.empty()) {
          prompt = "[collaborator directive]\n" + directive +
                   (prompt.empty() ? "" : "\n\n" + prompt);
        }
        // Consumed before the launch rather than after it: a child that is
        // already reading these would otherwise be handed them twice. When the
        // launch does not happen they are written back below.
        MailRestore restore{collaborator_id, {}};
        if (operation == "followup") {
          restore.taken = TakeCollaboratorMail(collaborator_id);
          for (const QueuedMessage& queued : restore.taken) {
            if (!prompt.empty()) prompt += "\n\n";
            prompt += std::string("[queued guidance") +
                      (queued.from.empty() || queued.from == "parent"
                           ? ""
                           : " from " + queued.from) +
                      "]\n" + queued.text;
          }
        }
        if (prompt.empty()) {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: spawn or followup requires prompt or "
                             "queued guidance");
        }

        std::string mode = JsonValue(arguments, "mode",
                                     JsonValue(collaborator, "mode", "lean"));
        if (mode != "lean" && mode != "full") {
          return ToolFailure(ToolErrorCode::kInvalidArguments,
                             "error: mode must be lean or full");
        }
        const std::string requested = NormalizeModelId(JsonValue(
            arguments, "model", JsonValue(collaborator, "model", "")));
        SideRoute route =
            ResolveSubagentRoute(api, routes, providers, requested);
        std::string route_label = SubagentDiagnosticRoute(route, providers);
        if (route.unresolved &&
            route.selection.find('/') != std::string::npos &&
            !CanUseRawModel(api, route.selection)) {
          return ToolFailure(
              ToolErrorCode::kInvalidArguments,
              ChildAgentFailureReport(
                  route_label, ChildAgentFailureStage::kRouteResolution,
                  "unknown model route: " + TerminalSafe(route.selection)));
        }
        double remaining_budget = 0;
        int64_t remaining_token_budget = 0;
        if (std::optional<ToolResult> blocked = ChildAgentBudgetBlock(
                api, processes, remaining_budget, remaining_token_budget)) {
          return *blocked;
        }
        const std::string child_model = route.model;
        EnvironmentOverrides environment =
            ChildAgentEnvironment(std::move(route));
        // Team identity travels with the child so same-team peers can reach
        // each other; the coordinator stamps Owner() at spawn and it is fixed.
        environment.emplace_back(
            "UAGENT_TEAM", JsonValue(collaborator, "team", processes.Owner()));
        // A caller that knows the shape of the subtask may raise or lower the
        // ceiling for that one child; the schema bounds it, and the session
        // budgets still apply underneath.
        const json& limits = persistent && operation == "followup"
                                 ? ChildLimits(collaborator)
                                 : ChildLimits(arguments);
        int64_t steps = JsonValue(limits, "steps", SubagentMaxSteps());
        int64_t tool_calls =
            JsonValue(limits, "tool_calls", SubagentMaxToolCalls());
        bool background =
            JsonValue(arguments, "background", persistent ? false : true);
        if (persistent && (background || !runtime)) {
          return ToolFailure(
              ToolErrorCode::kInvalidArguments,
              runtime ? "error: persistent handoffs are blocking"
                      : "error: persistent collaborators are unavailable");
        }
        // A caller may deny memory but not grant it: the session decides what
        // this process may read, and a child cannot widen that.
        bool child_memory = api.config.memory_enabled &&
                            JsonValue(limits, "memory",
                                      JsonValue(collaborator, "memory", true));
        environment.insert(
            environment.end(),
            {{"UAGENT_MAX_STEPS", std::to_string(steps)},
             {"UAGENT_MAX_TOOL_CALLS", std::to_string(tool_calls)},
             {"UAGENT_TOOLSET", mode},
             {"UAGENT_INTERNAL_PARENT_TURN", std::to_string(context.turn_id)},
             {"UAGENT_MEMORY", child_memory ? "1" : "0"},
             {"UAGENT_INTERNAL_SESSION_FILE",
              JsonValue(collaborator, "session_file", "")},
             // The parent brief is standalone. Re-inlining every always-on
             // memory in each child only duplicates context and emits a
             // misleading truncation warning when that optional cache is full.
             {"UAGENT_MEMORY_ALWAYS_BYTES", "0"}});
        // Only a background child is polled while it runs. A foreground child
        // is read once, where progress lines would only pad the answer the
        // parent quotes.
        if (background) {
          environment.emplace_back("UAGENT_HEADLESS_PROGRESS", "1");
        }
        // Tightening is the caller's to do; loosening is not. A requested
        // budget above what the session has left is clamped, and the clamp is
        // reported rather than applied behind the caller's back.
        std::vector<std::string> clamped;
        double child_budget = JsonValue(limits, "cost", 0.0);
        if (api.config.session_budget > 0) {
          if (child_budget <= 0 || child_budget > remaining_budget) {
            if (child_budget > remaining_budget) {
              clamped.push_back("limits.cost to " + FmtCost(remaining_budget) +
                                ", the session's remainder");
            }
            child_budget = remaining_budget;
          }
        }
        if (child_budget > 0) {
          environment.emplace_back("UAGENT_SESSION_BUDGET",
                                   std::to_string(child_budget));
        }
        if (api.config.session_token_budget > 0) {
          environment.emplace_back("UAGENT_SESSION_TOKEN_BUDGET",
                                   std::to_string(remaining_token_budget));
        }
        // The per-call budget bounds a command that might run away. A child
        // the caller chose to wait for is supervised, so it is bounded by
        // max_seconds when given and by the turn otherwise.
        ToolContext child_context = context;
        int64_t max_seconds = JsonValue(limits, "seconds", int64_t{0});
        int64_t ceiling = SubagentTimeoutSeconds();
        if (ceiling > 0 && (max_seconds <= 0 || max_seconds > ceiling)) {
          if (max_seconds > ceiling) {
            clamped.push_back("limits.seconds to " + std::to_string(ceiling) +
                              ", this build's ceiling");
          }
          max_seconds = ceiling;
        }
        if (max_seconds > 0) child_context = context.WithTimeout(max_seconds);
        // What this request says about the collaborator, recorded for both the
        // persistent and the bounded path once the child is under way.
        auto record_request = [&](const char* fallback_label) {
          collaborator["task"] = JsonValue(arguments, "prompt", "");
          collaborator["label"] = Utf8Trunc(
              FirstLine(JsonValue(arguments, "prompt", fallback_label)), 160);
          if (operation == "spawn" || arguments.contains("name")) {
            collaborator["name"] = JsonValue(
                arguments, "name", JsonValue(collaborator, "name", ""));
          }
          if (operation == "spawn" || arguments.contains("description")) {
            collaborator["description"] =
                Utf8Trunc(JsonValue(arguments, "description",
                                    JsonValue(collaborator, "description", "")),
                          kAgentDescriptionMax);
          }
          collaborator["memory"] = child_memory;
          collaborator["updated_at"] = UtcStamp();
        };
        if (persistent) {
          if (operation == "spawn") {
            collaborator["limits"] = {{"steps", steps},
                                      {"tool_calls", tool_calls},
                                      {"seconds", max_seconds},
                                      {"cost", child_budget},
                                      {"memory", child_memory}};
          }
          collaborator["mode"] = mode;
          collaborator["model"] =
              requested.empty() ? DefaultSubagentModel(api) : requested;
          record_request("Sidekick");
          collaborator["route"] = route_label;
          ToolResult saved = SaveCollaborator(collaborator);
          if (!saved.Ok()) return saved;
          Options options;
          options.yolo = true;
          options.debug = debug;
          for (auto& [key, value] : environment) {
            options.overrides[key] =
                key == "UAGENT_USAGE_FILE"
                    ? CollaboratorSessionPath(collaborator_id) + ".usage.jsonl"
                    : value;
          }
          bool submitted = false;
          ToolResult result = runtime->Handoff(
              {.id = collaborator_id,
               .path = CollaboratorSessionPath(collaborator_id),
               .cwd = CanonicalCwd(),
               .title = JsonValue(collaborator, "label", "Sidekick"),
               .model = JsonValue(collaborator, "model", ""),
               .route = route_label,
               .options = std::move(options),
               .remaining_cost = child_budget,
               .remaining_tokens = remaining_token_budget,
               .parent_turn = context.turn_id},
              prompt, child_context, &submitted);
          // Submission transfers queued guidance to the retained conversation.
          // A later model or transport failure must not replay it.
          if (submitted) restore.taken.clear();
          json lifecycle = runtime->Snapshot(collaborator_id);
          if (!lifecycle.empty()) {
            collaborator["runtime_generation"] =
                JsonValue(lifecycle, "runtime_generation", "");
            collaborator["handoff_generation"] =
                JsonValue(lifecycle, "handoff_generation", uint64_t{0});
            collaborator["updated_at"] = UtcStamp();
            (void)SaveCollaborator(collaborator);
          }
          result.output +=
              "\n[collaborator " + collaborator_id +
              (lifecycle.empty() ? "; runtime unavailable]"
                                 : "; persistent runtime retained]");
          result.facts = {{"agent_id", collaborator_id}};
          return result;
        }
        std::string command = ChildAgentCommand(debug, prompt, child_model);
        ShellCommandResult child = RunShellCommand(
            processes, child_context,
            {.command = std::move(command),
             .background = background,
             .immediate = background,
             // Runs uagent itself, which writes ~/.uagent
             // state a confined child could not. Its own
             // commands inherit UAGENT_SANDBOX and are
             // confined one level down.
             .sandbox = false,
             .activity_kind = ActivityKind::kSubagent,
             .activity_label = route_label,
             .source_id = collaborator_id,
             .completion_notes = clamped,
             .activity_metadata = {{"label", Utf8Trunc(FirstLine(JsonValue(
                                                           arguments, "prompt",
                                                           "Subagent")),
                                                       160)},
                                   {"model", route_label}},
             .environment = std::move(environment)});
        const bool launched = child.launched;
        ToolResult result = std::move(child.result);
        if (child.wait_status && result.artifact) {
          // The child ran long enough for its log to outgrow the cap, so the
          // text here may begin inside the record it ends with.
          result.output = ChildAgentRecoverEnvelope(std::move(result.output),
                                                    result.artifact->path);
        }
        if (result.Ok()) {
          // A launch receipt is process-supervisor output, not a malformed
          // child answer. Only a process that actually completed can have a
          // headless envelope to unwrap; retained completion notes travel with
          // a background job and are added again to its final result.
          result.output =
              child.wait_status
                  ? ChildAgentAnswer(std::move(result.output), clamped)
                  : std::move(result.output) +
                        ChildAgentConstraintNotes(clamped);
        }
        if (!result.Ok() && result.status != CompletionStatus::kCancelled) {
          ChildAgentFailureStage stage =
              child.wait_status ? ChildAgentFailureStage::kExecution
                                : ChildAgentFailureStage::kSpawn;
          result.output =
              ChildAgentFailureReport(route_label, stage, result.output) +
              ChildAgentConstraintNotes(clamped);
        }
        if (launched) {
          restore.taken.clear();
          collaborator["mode"] = JsonValue(
              arguments, "mode", JsonValue(collaborator, "mode", "lean"));
          collaborator["model"] = requested;
          record_request("Subagent");
          ToolResult saved = SaveCollaborator(collaborator);
          if (saved.Ok()) {
            result.output += "\n[collaborator " + collaborator_id +
                             "; resume with subagent operation=followup]";
            if (!result.facts.is_object()) result.facts = json::object();
            result.facts["agent_id"] = collaborator_id;
          } else {
            result.output +=
                "\n[warning: collaborator metadata was not saved: " +
                TerminalSafe(saved.output) + "]";
          }
        }
        return result;
      });
  tool.clamped_arguments = {"limits.steps", "limits.tool_calls",
                            "limits.seconds"};
  // Same reasoning as run, scratch and activity: the per-call budget stops a
  // command running away, and a child the caller is waiting for is neither
  // unsupervised nor unbounded — limits.seconds and the turn bound it, and
  // Escape still returns immediately.
  tool.timeout_s = 0;
  tool.mutating = true;
  tool.capabilities = Capability(ToolCapability::kDelegate);
  tool.delegates = true;
  tool.retain_output = true;
  tool.available_in_lean = false;
  // Concurrency is enforced by the spawn path (RunShellCommand reserves an
  // activity slot bounded by MaxBackgroundJobs); this is only a runaway
  // ceiling.
  tool.max_calls_per_turn = SubagentCallsPerTurn();
  auto describe = [&api, &routes, &providers](const json& arguments) {
    std::string operation = JsonValue(arguments, "operation", "spawn");
    if (operation == "list") return std::string("list collaborators");
    std::string mode = JsonValue(arguments, "mode", "lean");
    std::string prompt = JsonValue(arguments, "prompt", "");
    std::string id = JsonValue(arguments, "agent_id", "");
    const json* ids = JsonArray(arguments, "agent_id");
    if (id.empty() && ids != nullptr && !ids->empty() &&
        (*ids)[0].is_string()) {
      id = (*ids)[0].get<std::string>();
      if (ids->size() > 1) id += ",+" + std::to_string(ids->size() - 1);
    }
    if (operation == "message") {
      if (JsonValue(arguments, "broadcast", false)) {
        return "[message team] " + prompt;
      }
      return "[message " + id + "] " + prompt;
    }
    std::string label = SubagentTargetLabel(
        api, routes, providers,
        NormalizeModelId(JsonValue(arguments, "model", "")));
    const std::string name = JsonValue(arguments, "name", "");
    if (!name.empty()) label = name + " · " + label;
    if (mode == "full") label += " · full";
    const bool persistent = JsonValue(arguments, "persistent", false);
    if (persistent) label += " · persistent";
    if (!JsonValue(arguments, "background", !persistent)) {
      label += " · foreground";
    }
    if (!id.empty()) label += " · " + id;
    return "[" + label + "] " + prompt;
  };
  // Rows and traces name the work; approval_preview keeps the full form with
  // the route and flags, since that is what a person approves.
  tool.summary = [describe](const json& arguments) {
    const std::string operation = JsonValue(arguments, "operation", "spawn");
    if (operation == "list" || operation == "message") {
      return describe(arguments);
    }
    const std::string name = JsonValue(arguments, "name", "");
    return (name.empty() ? std::string("Subagent") : name) + " · " +
           FirstLine(JsonValue(arguments, "prompt", ""));
  };
  tool.present = [](const json& arguments) {
    json parts = json::array();
    const std::string prompt = JsonValue(arguments, "prompt", "");
    if (!prompt.empty()) parts.push_back(CodePart(prompt, "markdown", "task"));
    for (json& part : GenericInputParts(arguments, {"prompt"})) {
      parts.push_back(std::move(part));
    }
    return parts;
  };
  tool.markdown_output = true;
  // The summary names the model and the brief; what it cannot show is the
  // authority handed over with them. The child runs with automatic approvals,
  // so approving the spawn approves every tool call that child then decides
  // to make -- and "always" is no narrower, because the approval key hashes
  // tool policy rather than these arguments.
  tool.approval_preview = [describe, &api](const json& arguments) {
    std::string preview = describe(arguments);
    std::string operation = JsonValue(arguments, "operation", "spawn");
    if (operation != "spawn" && operation != "followup") return preview;
    const bool full = JsonValue(arguments, "mode", "lean") == "full";
    preview +=
        "\n\u00b7 the child approves its own tool calls; it writes files and "
        "runs commands unattended";
    preview += std::string("\n\u00b7 toolset ") +
               (full ? "full: reading, editing and running, plus its own "
                       "children"
                     : "lean: reading and running, no file edits");
    const json& limits = ChildLimits(arguments);
    preview += "\n\u00b7 bounded by " +
               std::to_string(JsonValue(limits, "steps", SubagentMaxSteps())) +
               " steps, " +
               std::to_string(
                   JsonValue(limits, "tool_calls", SubagentMaxToolCalls())) +
               " tool calls" +
               (api.config.memory_enabled && JsonValue(limits, "memory", true)
                    ? ", memory on"
                    : ", memory off");
    preview +=
        "\n\u00b7 \"always\" covers every later subagent call, not "
        "this brief";
    return preview;
  };
  return tool;  // Spawns serialize; immediate-background children overlap.
}

}  // namespace uagent
