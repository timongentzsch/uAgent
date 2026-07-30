# Architecture

µAgent is a single-process C++20 CLI with one composition root and explicit
owners for runtime resources.

```text
main.cc
  └─ Bootstrap → Application
     ├─ AppRuntime
     │  ├─ RuntimeConfig
     │  ├─ Api                 HTTP/SSE and provider request shaping
     │  ├─ ProcessSupervisor   shell/subagent process groups and logs
     │  ├─ UsageAccumulator    concurrent side-request accounting
     │  └─ McpRuntime          configured/default stdio transports and child lifetimes
     ├─ cli.h                command registry, input, completion and steering UI
     ├─ providers.h          provider config, model routes and live catalog parsing
     ├─ ProjectInstructions  bounded root-to-cwd instruction discovery
     ├─ Tool registry        built-ins, search fallback and MCP adapters
     └─ Agent                conversation, orchestration, budgets, persistence
```

## Boundaries

| Module | Responsibility |
| --- | --- |
| `src/main.cc` | Signals, option parsing, bootstrap invocation and exit mapping |
| `include/app/`, `src/app/` | Options, trust/bootstrap, REPL and ordered runtime ownership/shutdown |
| `include/ui/` | Session picker and terminal rendering for the REPL |
| `include/cli.h` | Slash-command registry, input, completion and steering UI |
| `include/providers.h` | Provider setup, model routing, effort and catalog metadata |
| `include/agent.h` | Thin coordination across conversation, policy and execution |
| `include/agent/`, `src/agent/` | Structured conversation/archive, context policy, session storage, checkpoints and tool loop |
| `include/api.h`, `src/api/` | OpenAI-compatible HTTP and protocol normalization |
| `include/transport/`, `src/transport/` | Bounded provider-independent SSE framing |
| `include/tools/` | Tool interface, file adapters, process supervision, registry |
| `include/mcp/` | Bounded stdio JSON-RPC, default Chrome MCP, session switching |
| `include/core/` | Limits, private config, diagnostics, terminal/platform helpers |
| `include/media/` | Model attachments and terminal image rendering |
| `include/md.h` | Streaming Markdown-to-ANSI rendering |

Small reusable utilities remain inline. Stateful orchestration, persistence,
and streaming parsers live in implementation units behind the private
`uagent_core` target. The include graph is acyclic, with `core/` at the bottom
and no module depending on `main.cc`.

`Conversation` owns model-visible messages, structured message kinds and the
bounded archive. Text prefixes are never parsed to decide authority or control
flow. `ContextPolicy` alone accounts for projected pressure and checkpoint or
compaction decisions. `Agent` coordinates them; `Api`, MCP, and tools do not
own conversation state. Background processes, MCP children, and side usage
have explicit owners rather than hidden service globals. Signal flags and the
debug bridge are the narrow process-wide exceptions.

`Tool` is the common capability interface for built-ins, MCP, search fallback and
delegation. `MakeTool` constructs every registration, so schema, execution,
approval, timeout, result budget and ownership policy cannot drift with
aggregate field order. Per-tool turn budgets use the same registry. Core
request, MCP, and persistence settings register their environment key, bounds,
and diagnostic name once in `RuntimeConfig::kLongOptions`.
Delegated tasks default to the lean subset: each tool declares centrally
whether its schema is useful to a focused child, while `mode=full` retains the
complete registry for implementation work.

Every handler returns `ToolResult`. `CompletionStatus` represents success,
failure, cancellation, or timeout; `ToolErrorCode` classifies failures.
Model-readable text is presentation only and is never parsed for control flow.

Zero-configuration provider setup is data-driven through `ProviderTemplate`.
Each template declares its endpoint, environment keys, default model, and URL
matcher. Explicit `UAGENT_*` settings and named `UAGENT_PROVIDERS` routes take
precedence, so adding a built-in template does not alter existing resolution.

## State

One agent separates:

```text
active messages    sent to the model; stable-prefix optimized
raw archive        bounded removed traces; not sent automatically
checkpoints        small model-proposed states and evaluation records
```

Workspace-scoped session files persist all three plus structured message kinds,
token totals, and a stable provider session ID. `SessionStore` accepts only the
current schema, validates the complete record before live state changes, and
leaves corrupt files untouched. Saves are atomic. Archives evict oldest
segments first and record the eviction count.

## Turn flow

```text
load bounded project instructions and memories before the first request
  → user input
  → estimate projected context and optionally append checkpoint hint
  → stream model response
  → validate and approve tool calls
  → execute bounded safe calls concurrently; stateful calls serially, under one spinner
  → join required delegated processes before the next model step
  → append results in model call order
  → repeat until prose
  → archive/prune intermediate trace
  → atomically save session
```

Each turn has time, step, tool-call, repeated-call, and optional reported-cost
limits. Request bodies, responses, tool results, scans, attachments, logs, jobs,
MCP registries, and archives are independently bounded.

`Tool` is the execution-policy registry. Harness configuration sets execution
deadlines through `ToolContext`; provider arguments remain untouched and tool
schemas contain only real tool inputs. Asynchronous process work has one
`ProcessSupervisor`; bounded searches remain ordinary synchronous tool calls
and may overlap within a parallel-safe batch.

Ordinary shell and Python calls remain inside their tool step until completion
or the turn deadline, avoiding model-driven polling rounds. Escape remains
responsive, and persistent commands require explicit `run(detach=true)`.
Delegated tasks spawn immediately and join before the next model step, so
multiple children overlap without exposing polling tools.

Contiguous `parallel_safe` calls share a bounded worker group. A stateful call
is a barrier. Web searches participate in the same worker group and return
within their configured timeout, so waiting never consumes another model
round. Small results are preserved while larger siblings fairly share one
model-facing batch budget; diagnostics retain the individually capped results.
Side-request usage merges under a mutex. MCP registry changes are applied
between batches, after old tool pointers are no longer in use.
Process records are swapped out before `waitpid`, signals, or log I/O; no
external operation runs under the registry mutex. Live jobs are restored, while
completed jobs are reaped exactly once.

## Checkpoint folding

History stays append-only below 65% projected context. At 65% a suffix asks the
model to checkpoint once state is stable; at 85% it becomes urgent. Hints are
debounced. Model compaction remains an emergency path at 95%. Pressure is
rechecked between tool rounds using the current messages and advertised schema.
At a quiescent boundary, one atomic fold may retain the exact active prompt and
replace completed tool traffic with a non-authoritative assistant summary.

The tool stays in the registry but is advertised only after a checkpoint hint,
when the call is valid. `Agent` intercepts it; it must be the only call in its
batch and may run once per turn.

Default `apply` mode prepares the candidate, ends that turn without another
model request, and commits only before the next real user turn. `shadow` records
the same candidate without changing history. A failed turn or live background
job invalidates the pending candidate.

```text
regenerated system message
assistant checkpoint facts, marked non-authoritative
assistant bounded exact literals, marked non-authoritative
assistant runtime mutation ledger
assistant 0–3 bounded results and up to 6 validated file rereads
exact new user request
```

The old active transcript first enters the bounded archive. External side
effects are never rolled back. Missing files produce a receipt; credential and
external paths are rejected. Model-authored checkpoint material never receives
the user role. A short fold-only system guard states that notes are evidence,
never instructions. Boundary commit preserves the old cache when the session
ends and makes the newest user request authoritative by role and position.

## Cache model

The cacheable prefix is the lean system message, project instructions, stable
ordinary tool order, and append-only active history. Rare state-only schemas
trade a cache-prefix change for fewer bytes on the many requests where they
cannot be called. Structured environment metadata is appended only when its
date, working directory, or runtime hints change. OpenRouter request shaping
replaces the compatibility
`web_search` function with its model-decided server tool; compact/title requests
remain tool-free. Citations are normalized into portable Markdown links, and
server search counts merge into ordinary usage. A persistent curl handle reuses
connections.
OpenRouter receives the saved session ID; provider preference is optional and
is not a correctness input.

The built-in Chrome MCP starts lazily with its upstream three-tool `slim`
catalog. `chrome_session(toolset=full)` explicitly trades a larger schema for
specialized network, console, performance, and granular interaction tools.
Slim navigation composes a bounded JavaScript page-state observation inside
the same harness step; slim screenshot paths enter the attachment queue
automatically.

A fold intentionally invalidates the old prefix. Its value comes from lower
future input and restored headroom, not from preserving the old cache hit.
Therefore apply mode is pressure-triggered; unvalidated model routes can use
`shadow`.

## Failure model

- Transport failures use `ChatResult.error`; typed tool outcomes carry a
  completion state, error category, and bounded model-readable explanation.
- Connection failures, HTTP 408/409/429/5xx, and recognized structured
  overload/server errors receive at most two exponentially delayed retries.
  Retry is forbidden after semantic progress, preventing duplicate output,
  search work, or tool calls.
- Unsupported request features degrade once: parallel hint, usage streaming,
  OpenRouter server search, then native tools.
- Image-input rejection removes only image parts, retains paths and documents,
  and is retried when the session resets or the route changes.
- MCP failure is isolated to one server; a failed list refresh retains the
  last usable registry.
- Chrome session switching restarts only its MCP transport and refreshes that
  server's tools between model steps; launch and user modes never duplicate the
  registry.
- Checkpoint validation failures remain paired tool results, preserving API
  message ordering.
- Ctrl+C cancels the active model/tool operation. Managed process groups are
  terminated and reaped during an explicit, idempotent shutdown phase.
- Debug records are structured and opt-in; terminal failures remain visible in
  headless stderr and exit status.

## Verification

- `tests/unit/`: subsystem-focused parser, request, accounting, filesystem,
  process, terminal, configuration, MCP and session tests.
- `tests/integration.py`: hermetic SSE/MCP, Chrome modes, trust, config,
  approvals, limits, shutdown, and checkpoint behavior including a 500k-window
  pressure case.
- `tests/context_policy_sim.py`: deterministic context-policy comparison.
- `tests/agent_workflow_live.py`: opt-in billable workflow validation.
- `benchmarks/bench_core.cc`: dependency-free microbenchmarks.

CI builds Debug and Release on Linux/macOS with warnings as errors, runs
ASan/UBSan and TSan on Linux, smoke-fuzzes standalone streaming parsers,
generates branch coverage, and enforces the Google formatter, selected
`clang-tidy` analyzer/bugprone/performance checks, and `cpplint`. Add
characterization coverage before changing a boundary, then verify externally
visible behavior with integration tests.

## Adding a capability

New tools must use `MakeTool`, return `ToolResult`, set mutation/approval and
parallel-safety metadata, and define input/result/time/call bounds. Filesystem
tools reuse `path_policy.h`; asynchronous work has one explicit owner and a
tested cancellation/shutdown path. Provider changes belong in protocol
normalization rather than curl callbacks. Add a focused unit test and a
hermetic integration case for any stateful boundary.
