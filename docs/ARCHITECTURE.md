# Architecture

µAgent is a single-process C++20 CLI with one composition root and explicit
owners for runtime resources.

```text
main.cc
  ├─ AppRuntime
  │  ├─ RuntimeConfig
  │  ├─ Api                 HTTP/SSE and provider request shaping
  │  ├─ ProcessSupervisor   shell/subagent process groups and logs
  │  ├─ UsageAccumulator    concurrent side-request accounting
  │  └─ McpRuntime          configured/default stdio transports and child lifetimes
  ├─ cli.h                command registry, input, completion and steering UI
  ├─ providers.h          provider config, model routes and live catalog parsing
  ├─ ProjectInstructions    bounded root-to-cwd AGENTS.md and CLAUDE.md discovery
  ├─ Tool registry          built-ins, OpenRouter search, MCP and Chrome adapters
  └─ Agent                  conversation, orchestration, budgets, persistence
```

## Boundaries

| Module | Responsibility |
| --- | --- |
| `src/main.cc` | Runtime composition, argument parsing, REPL dispatch |
| `include/ui/` | Session picker and terminal rendering for the REPL |
| `include/cli.h` | Slash-command registry, input, completion and steering UI |
| `include/providers.h` | Provider setup, model routing, effort and catalog metadata |
| `include/agent.h` | Model/tool loop, active history, checkpoints, sessions |
| `include/agent/` | Text-protocol fallback, system prompt, tool-call dispatch |
| `include/api.h` | OpenAI-compatible HTTP/SSE; no tool execution |
| `include/tools/` | Tool interface, file adapters, process supervision, registry |
| `include/mcp/` | Bounded stdio JSON-RPC, default Chrome MCP, session switching |
| `include/core/` | Limits, private config, diagnostics, terminal/platform helpers |
| `include/media.h` | Attachment encoding and terminal image protocol |
| `include/md.h` | Streaming Markdown-to-ANSI rendering |

Headers are grouped by subsystem and stay header-only; the include graph is
acyclic, with `core/` at the bottom and no module depending on `main.cc`.

`Agent` alone may replace model-visible history. `Api`, MCP, and tools do not
own conversation state. Background processes, MCP children, and side usage have
explicit owners rather than hidden service globals. Signal flags and the debug
bridge are the narrow process-wide exceptions.

`Tool` is the common capability interface for built-ins, MCP, Chrome, search,
and delegation. `MakeTool` constructs every registration, so schema, execution,
approval, timeout, result budget and ownership policy cannot drift with
aggregate field order. Per-tool turn budgets use the same registry. Core
request, MCP, and persistence settings register their environment key, bounds,
and diagnostic name once in `RuntimeConfig::kLongOptions`.

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

Workspace-scoped session files persist all three plus token totals and a stable
provider session ID. Saves are atomic. Archives evict oldest segments first and
record the eviction count.

## Turn flow

```text
load bounded project instructions and memories before the first request
  → user input
  → estimate projected context and optionally append checkpoint hint
  → stream model response
  → validate and approve tool calls
  → execute bounded safe calls concurrently; stateful calls serially
  → inject completed background side requests between model steps
  → append results in model call order
  → repeat until prose
  → archive/prune intermediate trace
  → atomically save session
```

Each turn has time, step, tool-call, repeated-call, and optional reported-cost
limits. Request bodies, responses, tool results, scans, attachments, logs, jobs,
MCP registries, and archives are independently bounded.

`Tool` is the execution-policy registry: its default plus the global fallback
centrally adds a model-overridable `timeout` to every schema. Handlers receive
the resulting deadline through `ToolContext`; background-safe work reports via
the session-owned supervisor instead of blocking a tool batch.

Background work declares whether it must join before a final answer. Prose is
provisional while required work remains; the runtime waits within the turn
deadline, injects every result, and resumes the model. Searches and subagents
join; intentionally long-lived shell/Python jobs do not. A shared job-id
interface lets `wait_background` join process and in-process side work alike.
Each process wait reports current output immediately, then returns on new output
or exit. The turn deadline and global call budget bound silent or noisy jobs.

Contiguous `parallel_safe` calls share a bounded worker group. A stateful call
is a barrier. Side-request usage merges under a mutex. MCP registry changes are
applied between batches, after old tool pointers are no longer in use.

## Checkpoint folding

History stays append-only below 65% projected context. At 65% a suffix asks the
model to checkpoint once state is stable; at 85% it becomes urgent. Hints are
debounced. Legacy model compaction remains an emergency path at 95%.

The tool is always registered to keep schema bytes stable and is intercepted
by `Agent`. It must be the only call in its batch, may run only after a hint,
and may run once per turn.

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
ordered tool schemas, and append-only active history. A persistent curl handle reuses connections.
OpenRouter receives the saved session ID; provider preference is optional and
is not a correctness input.

A fold intentionally invalidates the old prefix. Its value comes from lower
future input and restored headroom, not from preserving the old cache hit.
Therefore apply mode is pressure-triggered; unvalidated model routes can use
`shadow`.

## Failure model

- Transport failures use `ChatResult.error`; tool failures return bounded
  model-readable errors.
- Unsupported request features degrade once: parallel hint, usage streaming,
  then native tools.
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
  terminated and reaped on normal shutdown.
- Debug records are structured and opt-in; terminal failures remain visible in
  headless stderr and exit status.

## Verification

- `tests/test_core.cc`: parsers, request shaping, accounting, file/process and
  terminal boundaries.
- `tests/integration.py`: hermetic SSE/MCP, Chrome modes, trust, config,
  approvals, limits, shutdown, and checkpoint behavior including a 500k-window
  pressure case.
- `tests/context_policy_sim.py`: deterministic context-policy comparison.
- `tests/agent_workflow_live.py`: opt-in billable workflow validation.
- `benchmarks/bench_core.cc`: dependency-free microbenchmarks.

CI builds Debug and Release on Linux/macOS with warnings as errors, runs
ASan/UBSan on Linux, and enforces the Google formatter, `clang-tidy`, and
`cpplint`. Add characterization coverage before changing a boundary, then
verify externally visible behavior with integration tests.
