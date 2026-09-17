// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_TOOL_H_
#define UAGENT_INCLUDE_TOOLS_TOOL_H_
// The Tool type and registry machinery. Each Tool bundles a canonical function
// schema with its handler; wire adapters translate it at the API boundary.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/outcome.h"
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

// Single spelling of the human-facing failure prefix. ToolFailure and its
// siblings do not auto-prepend: existing call sites already carry the prefix
// and mass rewording would churn golden outputs. New code should build
// messages via ToolErrorText so the envelope stays greppable in one place.
inline constexpr std::string_view kToolErrorPrefix = "error: ";

std::string ToolErrorText(std::string_view message);

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

struct ToolResult {
  CompletionStatus status = CompletionStatus::kSuccess;
  std::string output;
  ToolErrorCode error = ToolErrorCode::kNone;
  std::optional<ToolArtifact> artifact;
  std::optional<ReadRange> read_range;
  // Optional model-facing override for this call. Most tools inherit their
  // registry cap; a bounded richer result can raise it.
  int64_t result_chars = -1;
  std::string display;     // optional terminal-only receipt
  bool no_change = false;  // activity poll found nothing new
  bool activity_terminal = false;

  bool Ok() const { return status == CompletionStatus::kSuccess; }
};

ToolResult ToolSuccess(std::string output, int64_t result_chars = -1);
ToolResult ToolFailure(ToolErrorCode error, std::string output);
ToolResult ToolCancelled(std::string output);
ToolResult ToolTimedOut(std::string output);

struct ToolContext {
  ToolContext() = default;
  explicit ToolContext(std::chrono::steady_clock::time_point deadline_value)
      : deadline(deadline_value) {}

  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  int64_t timeout_s = 0;
  std::string call_id;

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

struct Tool {
  using Run = std::function<ToolResult(const json&, const ToolContext&)>;
  using Summary = std::function<std::string(const json&)>;
  using Approval = std::function<bool(const json&)>;
  using Validate = std::function<std::optional<ToolArgumentIssue>(const json&)>;
  using Canonicalize = std::function<void(json&)>;
  // How a call must be authorised. `kMandatoryHuman` exists because some
  // targets change µAgent's own authority or limits: yolo, UAGENT_APPROVAL and
  // /yolo do not apply to it, and a non-interactive session denies rather than
  // assuming consent.
  using Classify = std::function<ApprovalClass(const json&)>;
  // Full, possibly multi-line text shown only when asking a person to approve
  // this call. `summary` stays a one-liner for labels, traces and evidence.
  using Preview = std::function<std::string(const json&)>;

  std::string name;
  std::string description;
  json parameters;               // JSON-schema for the args
  bool mutating = false;         // gated behind user approval
  bool declared_intent = false;  // presentation only, never authority
  Approval mutates;  // argument-dependent mutation (e.g. memory save)
  Run run;
  Canonicalize canonicalize;  // materialized provider args -> operation args
  Validate validate;          // semantic issue before approval/execution
  Summary summary;            // args -> one-line display
  bool redact_invalid_arguments = false;  // hide raw rejected arguments
  bool parallel_safe = false;             // safe beside another tool call
  uint32_t capabilities = kAllToolCapabilities;  // required to expose
  Approval needs_approval;   // dynamic policy (e.g. external read)
  Classify approval_class;   // escalates specific arguments
  Preview approval_preview;  // long-form text for the approval prompt
  // Why this call needs a person, when the reason is not the default one of
  // reaching µAgent's own configuration. Shown in the approval headline.
  std::string mandatory_reason;
  std::string provider;             // owner for live registry refresh
  json output_schema;               // optional MCP output contract
  std::string stable_argument;      // value must stay fixed during one turn
  int64_t timeout_s = -1;           // -1 = global default; 0 = turn limit
  int64_t result_chars = -1;        // -1 = global result cap
  int64_t max_calls_per_turn = -1;  // -1 = global turn budget
  // Numeric pacing hints — a yield, a context radius, a page size — where a
  // value outside the schema's bounds means "as much as allowed", not a
  // mistake. Clamping them into range costs nothing; rejecting spends a whole
  // model round to say the same thing, and these are the arguments models
  // overshoot most often.
  std::vector<std::string> clamped_arguments;
  bool available_in_lean = true;  // omit implementation-only schemas in tasks
  bool retain_output = false;     // keep durable procedure/state in context
  bool dedupe_output = false;     // collapse verified recent duplicate output
  bool command_policy = false;    // receives the approved-command allowlist
  bool delegates = false;         // contributes delegation runtime context
  bool memory_store = false;      // retained only when memory is enabled
  int64_t blocking_wait_default_ms =
      -1;  // >=0: wait_ms is intentional blocking
  enum class Visibility {
    kAlways,
    kDetachedTerminal,
  };
  Visibility visibility = Visibility::kAlways;
};

struct ToolAvailability {
  bool detached_terminal = false;
};

struct ToolPolicy {
  uint32_t allowed = kAllToolCapabilities;
  std::vector<std::string> tool_allowlist;
  std::vector<std::string> run_allowlist;
  std::string error;
};

ToolPolicy ToolPolicyFromEnvironment();
void ApplyToolPolicy(std::vector<Tool>& tools, const ToolPolicy& policy);

Tool MakeTool(std::string name, std::string description, json parameters,
                     Tool::Run run);

Tool& AddTool(std::vector<Tool>& tools, Tool tool);

bool ToolMutates(const Tool& tool, const json& arguments);

bool ToolCallBlocks(const Tool& tool, const json& arguments);

// Contract-defined for native operations; arbitrary execution may declare its
// purpose. Neither this label nor a successful exit proves absence of effects.
std::string ToolActivityCategory(const Tool& tool, const json& args);

// The authority a call needs. A tool may escalate specific arguments; nothing
// can de-escalate below what the tool itself declares.
ApprovalClass RequiredApproval(const Tool& tool, const json& arguments);

void KeepLeanTools(std::vector<Tool>& tools);

std::string ToolDescription(const Tool& tool);

json ToolParameters(const Tool& tool);

// One-line display for a call: the tool's own formatter, else `path` (the
// common case), else the raw args. Shared by the approval prompt and the
// call trace so both name the same action the same way.
std::string ToolSummary(const Tool& t, const json& args);

const Tool* FindTool(const std::vector<Tool>& tools, const std::string& name);

// Structured argument validation against a tool's JSON schema.
std::optional<ToolArgumentIssue> FindToolArgumentIssue(const Tool& tool,
                                                       const json& args);

// Compatibility helper for callers that only need the human-readable error.
std::string InvalidToolArgument(const Tool& tool, const json& args);

// Pull the tool's `clamped_arguments` back inside their schema bounds. Runs
// before validation, so an overshooting hint is honoured at the bound. Each
// reduction is appended to `clamped` when given, because a caller that asked
// for more than it got is owed both numbers rather than a quiet substitution.
void ClampToolArguments(const Tool& tool, json& args,
                        std::vector<std::string>* clamped = nullptr);

// Apply a tool's provider-materialization cleanup and numeric clamps to the
// separate execution copy of its arguments.
void CanonicalizeToolArguments(const Tool& tool, json& args,
                               std::vector<std::string>* clamped = nullptr);

// A tool's `stable_argument` must keep the same value for a whole turn.
// `values` carries that per-turn memory for the caller.
std::string StableArgumentError(
    const Tool& tool, const json& args,
    std::unordered_map<std::string, std::string>& values);

json ToolSchema(const Tool& tool);

// registry -> the `tools` array for a chat request
json ToolSchemas(const std::vector<Tool>& tools);

class ToolSchemaCache {
 public:
  void Reset() { valid_ = false; }

  // Availability selection lives in tools/policy.cc so every translation
  // unit including this header does not compile it again.
  const json& Get(const std::vector<Tool>& tools, const json& schemas,
                  const std::unordered_map<std::string, int64_t>& counts,
                  ToolAvailability availability = {});

  size_t Bytes() const { return bytes_; }
  const json& Schemas() const { return available_; }

 private:
  bool valid_ = false;
  size_t bytes_ = 0;
  std::vector<size_t> selected_;
  json available_ = json::array();
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_TOOL_H_
