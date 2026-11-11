// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_TOOL_H_
#define UAGENT_INCLUDE_TOOLS_TOOL_H_
// The Tool type and registry machinery. Each Tool bundles a canonical function
// schema with its handler; wire adapters translate it at the API boundary.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/outcome.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {

enum class ToolErrorCode {
  kNone,
  kInvalidArguments,
  kPermissionDenied,
  kNotFound,
  kLimitExceeded,
  kUnavailable,
  kProcessFailed,
  kRemoteError,
  kInternal,
};

const char* ToolErrorCodeName(ToolErrorCode code);

// ToolFailure prepends it when missing; ToolCancelled and ToolTimedOut do not.
inline constexpr std::string_view kToolErrorPrefix = "error: ";

struct ToolArgumentIssue {
  std::string code;
  std::string message;
  std::string field;
};

ToolArgumentIssue ArgumentIssue(std::string code, std::string message,
                                std::string field = {});

struct ToolArtifact {
  std::string path;
  uint64_t bytes = 0;
};

std::string ArtifactHint(const ToolArtifact& artifact);

struct ReadRange {
  std::string path;
  int64_t first = 0, last = 0;
};

// A file the call changed, for the session's undo (EditJournal). `before` is
// unset when the bytes were not kept; `after_hash` is empty once deleted.
struct FileEffect {
  std::string path;
  bool existed = false;
  std::optional<std::string> before;
  std::string after_hash;
  // Lets undo recreate a deleted file with its original permissions.
  std::optional<uint32_t> before_mode = std::nullopt;
};

// What a result was, as facts every client words the same way (ResultWord).
// An unset fact was not reported, which is not the same as zero.
struct ResultFacts {
  struct Exit {
    int code = 0;
    int signal = 0;  // the signal that ended it; `code` then means nothing
  };
  std::optional<Exit> exit = std::nullopt;
  std::optional<int64_t> added = std::nullopt;    // lines a change added
  std::optional<int64_t> removed = std::nullopt;  // and removed
  std::optional<int64_t> items = std::nullopt;    // what the result counts
  std::string unit = {};  // the singular noun `items` counts: "line"
  // Why a call ended as it did beyond its state: denied, rejected, invalid,
  // limit, unavailable, timed_out. The tool loop sets it.
  std::string outcome = {};
  std::string reason = {};  // a short why, for a refused call
  bool no_change = false;   // it found or changed nothing new
};

// A background command a call saw end; the row of its starting call takes it.
struct EndedCommand {
  int64_t activity_id = 0;
  int status = 0;  // its wait status
  std::string output = {};
  double duration_ms = 0;
  bool stopped = false;  // a stop ended it, not the command itself
};
// As a remote gateway reports one to the main.
json EndedJson(const EndedCommand& ended);
EndedCommand EndedFromJson(const json& value);

struct ToolResult {
  CompletionStatus status = CompletionStatus::kSuccess;
  // What the call returned: the model reads it and a person sees it.
  std::string output = {};
  // For the model only: the tool loop appends it to `output`.
  std::string hint;
  ResultFacts facts;
  // How a client draws the body: text, markdown, tail (its last lines stay
  // visible), diff (`diff` is the body), image, link or fields.
  std::string body_kind = "text";
  ToolErrorCode error = ToolErrorCode::kNone;
  std::optional<ToolArtifact> artifact;
  std::optional<ReadRange> read_range;
  // Overrides the tool's result cap for this call; -1 inherits it.
  int64_t result_chars = -1;
  std::string diff;  // what a change did, as diff lines; never model-facing
  // The whole diff, once the turn loop has cut `diff` to its opening.
  std::string diff_path;
  // Display-only parts shown on the row, never model-facing.
  json parts = nullptr;
  std::optional<FileEffect> effect;
  // The to-do list as the call leaves it, [{text, status}]; null keeps it.
  json todo = nullptr;
  // The skill this call opened, which unlocks its tools (Tool::skill).
  std::string opened_skill;
  // Identity of a failed exact edit: target, original revision and old text.
  std::string recovery_key;
  bool activity_terminal = false;
  // The activity of a command this call left running; its row runs until then.
  int64_t background = 0;
  // Background commands this call saw end.
  std::vector<EndedCommand> ended;

  bool Ok() const { return status == CompletionStatus::kSuccess; }
};

ToolResult ToolSuccess(std::string output, int64_t result_chars = -1);
ToolResult ToolFailure(ToolErrorCode error, std::string output);
ToolResult ToolCancelled(std::string output);
ToolResult ToolTimedOut(std::string output);
// A success when `wait_status` is a clean exit, else a failure with its hint.
ToolResult ExitResult(std::string output, int wait_status);
// "[exit code N]" or "[killed by signal N]"; "" on a clean exit unless show_ok.
std::string ExitHint(int wait_status, bool show_ok);
// How `wait_status` ended a process, as a fact; unset when it did not end.
std::optional<ResultFacts::Exit> ExitFacts(int wait_status);
// A change: `output` tells the model, `diff` shows a person.
ToolResult ChangeResult(std::string output, std::string diff, int64_t added,
                        int64_t removed);
ToolResult ListResult(std::string output, int64_t items, std::string unit);
// Adds a line to the model's hint; leading newlines of `line` are dropped.
void AddHint(ToolResult& result, std::string_view line);
// `result` as the model reads it: its output, then its hints.
std::string ModelText(const ToolResult& result);
// What follows the output whatever cap cuts it: the hint, then the log's path.
std::string ModelHint(const ToolResult& result);

// The state vocabulary of rows and receipts: ok, failed, cancelled, running.
const char* ResultState(CompletionStatus status);
// "running" while the command the call left in the background runs.
const char* RowState(const ToolResult& result);
// How a row's result reads, with its duration when >= 0: "exit 2 · 1.4s".
std::string ResultWord(const ResultFacts& facts, std::string_view state,
                       double duration_ms);
// The facts as a row carries them, with their word.
json ResultJson(const ResultFacts& facts, std::string_view state,
                double duration_ms);

struct ToolContext {
  ToolContext() = default;
  explicit ToolContext(std::chrono::steady_clock::time_point deadline_value)
      : deadline(deadline_value) {}

  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  int64_t timeout_s = 0;
  std::string call_id;
  int64_t turn_id = 0;
  // Mail hops since the person's message; sessions stop at kMailMaxHops.
  int hops = 0;
  // Calls in this batch that start agents; they share what the session has.
  int64_t delegations = 1;
  // What this call's approval covered (Tool::approval_subject); null without.
  json approved;
  // Updates the running call label without adding transcript messages.
  std::function<void(const std::string&)> progress;

  bool Expired() const;

  ToolContext WithTimeout(int64_t seconds) const;

  int64_t RemainingSeconds(int64_t configured) const;
};

enum class ToolCapability : uint32_t {
  kInspect = 1U << 0,
  kExecute = 1U << 1,
  kMutate = 1U << 2,
  kDelegate = 1U << 3,
  kExternal = 1U << 4,
};

inline constexpr uint32_t Capability(ToolCapability capability) {
  return static_cast<uint32_t>(capability);
}

inline constexpr uint32_t kAllToolCapabilities =
    Capability(ToolCapability::kInspect) |
    Capability(ToolCapability::kExecute) | Capability(ToolCapability::kMutate) |
    Capability(ToolCapability::kDelegate) |
    Capability(ToolCapability::kExternal);

enum class ApprovalClass {
  kNone,
  kYoloEligibleMutation,
  kMandatoryHuman,
};

// The fields each value of a multi-action tool's discriminating argument
// needs, which one flat schema cannot say.
struct ActionRequires {
  std::string field;  // the discriminating argument: action or operation
  std::vector<std::pair<std::string, std::vector<std::string>>> actions;
};

// A call's headline: "Editing a.ts" while it runs, "Edited a.ts" after.
struct Headline {
  std::string present;
  std::string past;
  std::string target = {};
  // Replaces all of the above on the row, where a target would read poorly.
  std::string label = {};
};

struct Tool {
  using Run = std::function<ToolResult(const json&, const ToolContext&)>;
  using Approval = std::function<bool(const json&)>;
  using Validate = std::function<std::optional<ToolArgumentIssue>(const json&)>;
  using Canonicalize = std::function<void(json&)>;
  // `kMandatoryHuman` is for targets that change µAgent's own authority or
  // limits: yolo does not apply to it and a non-interactive session denies.
  using Classify = std::function<ApprovalClass(const json&)>;
  // Full, possibly multi-line text shown only when asking a person to approve.
  using Preview = std::function<std::string(const json&)>;
  // What a remembered approval covers; unset means the exact arguments.
  using Subject = std::function<json(const json&)>;
  // A call's input as ToolView parts; unset means the generic view.
  using Present = std::function<json(const json&)>;
  // What a call reads as; unset means "Calling/Called <title>".
  using Headlines = std::function<Headline(const json&)>;

  std::string name;
  std::string category;  // workspace, execute, web, collaborate, memory, mcp
  // The skill that unlocks this tool: offered only once that skill is open.
  std::string skill;
  std::string description;
  json parameters;        // JSON-schema for the args
  bool mutating = false;  // gated behind user approval
  Approval mutates;       // argument-dependent mutation (e.g. memory save)
  // Calls that only observe earlier work: rows shown at full detail only.
  Approval routine;
  Run run;
  Canonicalize canonicalize;   // materialized provider args -> operation args
  Validate validate;           // semantic issue before approval/execution
  Headlines headline;          // args -> verb and target
  Present present;             // args -> ToolView input parts
  bool parallel_safe = false;  // safe beside another tool call
  uint32_t capabilities = kAllToolCapabilities;  // required to expose
  Approval needs_approval;   // dynamic policy (e.g. external read)
  Classify approval_class;   // escalates specific arguments
  Preview approval_preview;  // long-form text for the approval prompt
  Subject approval_subject;  // what a remembered approval covers
  // Why this call needs a person, shown in the approval headline.
  Preview mandatory_reason;
  std::string provider;             // owner for live registry refresh
  json output_schema;               // optional MCP output contract
  int64_t timeout_s = -1;           // -1 = global default; 0 = turn limit
  int64_t result_chars = -1;        // -1 = global result cap
  int64_t max_calls_per_turn = -1;  // -1 = global turn budget
  // The fields each action needs, beside `parameters`' own `required`.
  ActionRequires action_requires = {};
  // Numeric pacing hints where a value outside the schema's bounds means "as
  // much as allowed": clamped, not rejected.
  std::vector<std::string> clamped_arguments;
  bool retain_output = false;  // keep durable procedure/state in context
  bool dedupe_output = false;  // collapse verified recent duplicate output
  bool delegates = false;      // starts agents (ToolContext::delegations)
  bool memory_store = false;   // retained only when memory is enabled
  // Asked before every request: false withholds the tool while it cannot run.
  std::function<bool()> available;
  int64_t blocking_wait_default_ms =
      -1;  // >=0: wait_ms is intentional blocking
};

// A session-local soft filter: it only removes allowed tools from the model
// surface. Overrides are kept apart from the profile, which may change.
class ToolSelection {
 public:
  bool Enabled(const Tool& tool) const;
  bool Configure(const json& request, const std::vector<Tool>& tools,
                 std::string& error);
  void Restore(const json& value);
  json Save() const;
  json Catalogue(const std::vector<Tool>& tools) const;

 private:
  std::string profile_ = "default";
  std::optional<std::unordered_set<std::string>> only_;
  std::unordered_map<std::string, bool> overrides_;
};

struct ToolPolicy {
  uint32_t allowed = kAllToolCapabilities;
  std::string error;
};

ToolPolicy ToolPolicyFromEnvironment();
void ApplyToolPolicy(std::vector<Tool>& tools, const ToolPolicy& policy);

Tool MakeTool(std::string name, std::string description, json parameters,
              Tool::Run run);

Tool& AddTool(std::vector<Tool>& tools, Tool tool);

bool ToolMutates(const Tool& tool, const json& arguments);

bool ToolCallBlocks(const Tool& tool, const json& arguments);

// The authority a call needs: arguments may escalate it, never lower it.
ApprovalClass RequiredApproval(const Tool& tool, const json& arguments);

std::string ToolDescription(const Tool& tool);

json ToolParameters(const Tool& tool);

// The first short, one-line string argument; "" when there is none.
std::string DefaultTarget(const json& args);
// The tool's own headline, else the default; `tool` is null for a tool gone.
Headline ToolHeadline(const Tool* tool, const json& args);
// The call in one line, its past verb and target: "Ran make test".
std::string ToolSummary(const Tool& t, const json& args);

// What every client renders for a call: {"verb": [present, past], "target",
// "input": [part...]}. `tool` is null for a tool gone: the generic view.
json ToolView(const Tool* tool, const json& args);
json CommandPart(std::string text);
json CodePart(std::string text, std::string language, std::string label = "");
json LinkPart(std::string to, json id, std::string label);
// A headline with a fixed verb pair and the default target.
Tool::Headlines Verbs(std::string present, std::string past);
// Generic parts for `args`, leaving out the arguments named in `skip`.
json GenericInputParts(const json& args,
                       std::initializer_list<std::string_view> skip = {});

const Tool* FindTool(const std::vector<Tool>& tools, const std::string& name);

std::string ToolCategory(const Tool& tool);
std::string ToolTitle(const Tool& tool);

// The first way `value` breaks `schema`; `name` labels the root.
std::optional<ToolArgumentIssue> FindSchemaIssue(const json& schema,
                                                 const json& value,
                                                 const std::string& name);
// Checks `args` against the tool's schema, then its action_requires.
std::optional<ToolArgumentIssue> FindToolArgumentIssue(const Tool& tool,
                                                       const json& args);

// Pulls `clamped_arguments` into their schema bounds; `clamped` lists each cut.
void ClampToolArguments(const Tool& tool, json& args,
                        std::vector<std::string>* clamped = nullptr);

// Applies the tool's canonicalize and clamps to the execution copy of `args`.
void CanonicalizeToolArguments(const Tool& tool, json& args,
                               std::vector<std::string>* clamped = nullptr);

json ToolSchema(const Tool& tool);

// registry -> the `tools` array for a chat request
json ToolSchemas(const std::vector<Tool>& tools);

class ToolSchemaCache {
 public:
  void Reset() { valid_ = false; }

  // The tools offered depend on configuration alone, never on turn progress:
  // the tool array is part of the cached prompt.
  const json& Get(const std::vector<Tool>& tools, const json& schemas,
                  const ToolSelection* selection = nullptr);

  size_t Bytes() const { return bytes_; }
  const json& Schemas() const { return available_; }

 private:
  bool valid_ = false;
  size_t bytes_ = 0;
  std::vector<size_t> selected_;
  json available_ = json::array();
};

// Single-use approvals keyed by the exact tool arguments they were prepared
// from, so the preview a person approved is the request that runs. The
// arguments are re-compared, so the digest is an index, not the boundary.
template <typename T>
class ApprovedProposals {
 public:
  void Put(const json& arguments, T value,
           std::chrono::steady_clock::time_point expires) {
    if (entries_.size() >= kMaxApprovedProposals) entries_.clear();
    entries_[HashHex(JsonDump(arguments))] = {arguments, std::move(value),
                                              expires};
  }
  // Removes and returns the approval; empty when absent, expired or mismatched.
  std::optional<T> Take(const json& arguments) {
    auto found = entries_.find(HashHex(JsonDump(arguments)));
    if (found == entries_.end()) return std::nullopt;
    Entry entry = std::move(found->second);
    entries_.erase(found);
    if (entry.arguments != arguments ||
        std::chrono::steady_clock::now() > entry.expires) {
      return std::nullopt;
    }
    return std::move(entry.value);
  }

 private:
  struct Entry {
    json arguments;
    T value;
    std::chrono::steady_clock::time_point expires;
  };
  std::map<std::string, Entry> entries_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_TOOL_H_
