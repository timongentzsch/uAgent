# Architecture

µAgent is one C++20 coordinator binary with explicit resource owners, owned
child processes, and no runtime framework.

```text
main → Bootstrap → Application
                  ├─ Api + HTTP/SSE
                  ├─ Agent + Conversation
                  ├─ Tool registry + ProcessSupervisor
                  ├─ MCP runtime
                  ├─ Usage + session storage
                  └─ Observability
                       ├─ terminal presenter
                       ├─ uagent.event.v2 JSONL
                       ├─ debug JSONL
                       └─ bounded session journal
```

Provider adapters, the tool executor, the agent loop, and the runtime emit one
typed semantic event value. Fixed sinks observe that value; `Emit` returns
nothing and no sink can influence agent control flow. This is deliberately not
a service bus, service locator, or plugin system.

## Boundaries

| Path | Responsibility |
| --- | --- |
| `src/app/`, `include/app/` | options, bootstrap, REPL, shutdown |
| `src/agent/`, `include/agent/` | conversation, compaction, and public agent ownership |
| `src/agent/turn_internal.h`, `src/agent/turn.cc` | private turn limits, metrics, stop causes, and step strategy |
| `src/api/`, `include/api/` | canonical request options, wire adapters, HTTP, and SSE decoding |
| `src/providers.cc`, `include/providers.h` | provider catalogue and route activation |
| `src/tools/process_io.cc`, `include/tools/process.h` | process ownership, checked activity transitions, and event-driven I/O |
| `src/tools/activity_log.cc`, `activity_completion.cc`, `jobs.cc` | log/detach persistence, exact-once completion, and activity interactions |
| `src/mcp/`, `include/mcp/` | bounded stdio JSON-RPC integrations |
| `include/core/` | configuration registry, usage, diagnostics, platform helpers |
| `include/ui/`, `include/cli.h` | inline terminal rendering and input |

`Conversation` owns model-visible messages and the bounded archive. Its
canonical role shape has exactly one `system` message at index zero, carrying
the system prompt plus project instructions and the memory index. Every later
harness injection (runtime context, advisories, text-protocol tool results)
rides as a `user` turn, and native tool results keep `tool`. Wire adapters map
that representation to Chat Completions messages, Responses input items, or
Anthropic content blocks. Opaque reasoning and hosted-tool replay data is kept
under one internal assistant-message field and emitted only when the same wire
API continues; a route switch strips it. `NormalizeRole` is the single place
that decides canonical provenance. `Agent` compares estimated/reported context
against one threshold; providers and tools do not own conversation state.
`ProcessSupervisor`, `McpRuntime`, and `UsageAccumulator` each own one class of
external resource.

Shared policy stays centralized: `MakeTool` defines tool metadata,
`RuntimeConfig` defines limits, `RouteKey` defines route identity, and
`HeadlessResult` defines machine output.

### Protocol and authority

Model text is never a native tool call. While native tools are enabled, only a
provider `tool_calls` object with a known name and valid object arguments can
reach dispatch. If a route explicitly rejects native tools, µAgent downgrades
that route and enables one compatibility syntax; a compatibility call must
occupy the entire assistant message. Unknown provider markup is detected only
to suppress and recover from a malformed response—it is never translated into
an executable call. This keeps detection broader than execution.

Conversation roles preserve provenance: native results remain `tool`, fallback
results are harness-owned context, and tool output has compatibility delimiters
escaped before it returns to the model. Files, tools, web pages, memories, MCP
responses, and model-authored summaries are untrusted evidence. They may inform
an authorized task but cannot grant authority or expand its scope. This is not
implemented as a semantic “prompt injection regex”; the enforceable boundary is
the typed protocol plus schema validation, tool policy, path checks, approval,
and process ownership. Prompt wording is defense in depth.

Web search has two explicit paths. In `auto`, a Responses or Anthropic route
gets its native hosted tool only when `hosted_tools` declares `web_search`;
otherwise µAgent exposes its separately configured OpenRouter search function.
`openrouter` forces the separate function and `off` exposes neither. No model
name, provider label, or URL implies hosted-tool support. Both paths normalize
citations and usage into the same turn accounting.

## Configuration is described once

One `constexpr` registry in `include/core/config_registry.h` describes every
`UAGENT_*` setting: default, bounds, category, when a change takes effect, and
whether the value may be displayed. Runtime getters resolve their descriptor at
compile time, `RuntimeConfig` binds descriptors to fields, diagnostics derive
redaction from declared sensitivity, and `uagent --emit-reference` generates the
bundled skill's documentation from the same table. A getter naming an
unregistered setting does not compile; a source-contract test also rejects
direct runtime `UAGENT_*` lookups unless they are registered or explicitly
named `UAGENT_INTERNAL_*`. CI fails when the generated references differ from
the registry, so a documented default cannot drift from the one the binary
applies.

`uagent_info` exposes that registry, the flag table, the slash-command table,
the live tool list and the prompt surface in effect as a read-only tool. It is assembled only when called,
adds nothing to the system prompt, and reports secrets as set or unset.

The model-facing surface is generated the same way. `--emit-reference` also
writes the base prompt with its capability fragments and the built-in tool
schemas, and the manifest carries a digest of each, so a reworded prompt
section or a changed argument description is a reviewable diff rather than an
invisible behavior change. Emission is a function of the source alone: no
environment, no live session. Per-session additions — host capabilities,
runtime context, project instructions, the mutable directive — are recorded by
`--debug` instead.

Prompt authoring lives in `include/agent/prompt.h` and `src/agent/prompt.cc` —
the base text, the conditional capability sections, the overlay and the runtime
context line. `include/agent/protocol.h` keeps tool-call parsing and detection
alone, so rewording the prompt rebuilds one translation unit rather than every
consumer of the protocol.

`UAGENT_PROMPT_OVERLAY` names a JSON file that replaces base-prompt sections,
so two prompt variants can be compared without rebuilding. It reaches prompt
text only: the capability, host-capability and directive sections below the
base are not addressable, and authority stays with tool policy, approval
classes and process ownership. An absent or malformed overlay leaves the
shipped prompt byte for byte, and an applied one is recorded next to the
request it shaped.

## Changing µAgent's own configuration

The agent has no tool that writes configuration. Editing `~/.uagent/.config`, a
project config, the trust store or `.mcp.json` through the built-in file tools
is classified `kMandatoryHuman`: `--yolo`, `UAGENT_APPROVAL=yolo` and `/yolo`
do not apply, the default answer is no, and a headless or delegated run denies
rather than assuming consent. That covers the built-in tools; the sandbox below
is what stops an approved shell command from reaching the same paths behind
them.

## Sandbox

Every command the agent runs is confined by the OS, and one chokepoint decides
how: `RunShellCommand` resolves a wrapper before it spawns anything, so a new
call site is confined by default rather than by remembering to ask. A session
that was told to confine and cannot does not spawn at all — a silent
unconfined command is the one outcome the sandbox exists to rule out.

The policy is composed once per session and is pure: `src/core/sandbox.cc` turns a
set of candidate roots into writable roots and an SBPL profile without touching
the filesystem, which is why both platforms' rules are unit-testable on either.
Canonicalisation happens before composition, in `src/core/sandbox_posix.cc`, because
seatbelt matches resolved paths only and because a root spelled `~/.uagent/..`
would otherwise pass the ancestor screen and resolve back to the home
directory.

macOS prepends `sandbox-exec -p`. Linux re-execs µAgent as `--sandbox-child`
beside `--log-pump`, passing the policy as separate argv words so no quoting
rule sits between parent and trampoline; the trampoline sets `no_new_privs`,
applies a Landlock ruleset, and execs. Every failure there exits without
executing. Landlock is monotonic — a nested µAgent can only narrow what it
inherited, never widen it — and a delegated child cannot reach the escape
hatch, which always requires a person.

Every route mutation uses one activation path: construct a centralized
`ProviderCapabilities` contract, export its stable child-process projection,
then rotate the agent route identity. The contract independently declares
`wire_api` (`chat_completions`, `responses`, or `anthropic_messages`) and
`hosted_tools`. Request serialization, model catalogues, reasoning replay,
search availability, and negotiated degradation read that contract rather
than provider/model names. Successful responses add observed reasoning,
citation, and usage facts without controlling the current turn.

## Turn

```text
user input
  → project instructions, explicit skills, and deferred memory index
  → context-pressure decision
  → streamed model response
  → validate and approve tool calls
  → run independent safe calls concurrently, stateful calls serially
  → append bounded results in call order
  → repeat until final text
  → archive trace, compact old bulky results in batches, atomically save
```

Network requests, idle streams, tool calls, output, processes, memory, and
context are bounded by default. Aggregate model rounds, tool calls, wall-clock
turn time, and reported cost have configurable opt-in caps; a zero
`UAGENT_MAX_TURN_SECONDS` leaves the complete turn unbounded while the
request/stream/tool deadlines remain active. Persistent commands require `run(detach=true)`. Delegated work runs in
separate sanitized processes. One `ProcessSupervisor` owns foreground commands,
background commands, tasks, and detached services. Session activities receive
opaque IDs distinct from OS PIDs. Persistent detached records pair their PID
with a boot-scoped kernel start identity before the process group is treated as
live or signalled.

`run` applies a configurable 10-second initial wait by default; explicit
`yield_ms=0` preserves full synchronous waiting. `tty=true` retains a POSIX PTY,
merged output, writable input, process-group signaling, and resize support.
Ctrl+B requests that every foreground command in the current tool batch yield
without being signaled or restarted, allowing queued steering to apply at the
next tool boundary. Persistent detached activities remain log-only and do not
retain interactive input after the harness exits.

A single process-I/O thread blocks on PTY/pipe readiness and a nonblocking
control pipe. `SIGCHLD` wakes a signal dispatcher that safely fans out to each
supervisor control pipe, so child reaping does not require a fixed-frequency
timer; only the bounded trailing-output grace creates a real deadline. The
thread feeds the private log, an incremental 1 MiB head/tail buffer, and an
aggregate bounded transcript, and it is the sole owner of process reaping and
I/O-FD closure. The buffer preserves the oldest and newest bytes
and reports an omitted middle. Each `activity` call explicitly selects list,
poll, wait, write, resize, or stop; the operation, rather than the presence of
an optional field, controls execution and approval. Interactions against one
activity serialize PTY writes, polling, interruption, and resize. An id-less
wait joins blocked work and yields on queued steering without cancelling it,
while `operation=stop` terminates the complete process group.
An exact live detached command and working-directory match returns the existing
activity rather than spawning a duplicate. Results arrive at model-step
boundaries. The persistent interactive loop and headless runner drain
completions and resume through an internal harness turn that does not count as
user input. Detached ownership follows the process group after its wrapper
exits.
The application owns a small raw-mode composer while the foreground agent runs
on one worker thread. Agent output is marshalled back above the two-line
composer, preserving native scrollback. A stateful decoder retains fragmented
CSI and bracketed-paste sequences across reads; the composer owns history and
paste bounds.
One helper paints the pinned region — erase, optional transcript text, status
row, composer — so its geometry is computed in a single place. SIGWINCH only
records a deadline: the repaint waits until resizes stop arriving, and the
status refresh stays silent until then, because a row formatted for one width
wraps if the terminal has already become narrower.
The application event loop blocks on stdin, captured output, worker requests,
activity notifications, and idle MCP stdout together. Its timeout is the
nearest real Escape-decoder, resize-settle, or 10 Hz status-animation deadline
rather than a background-completion tick. MCP request waits and curl multi transfers include
the pollable abort descriptor, while SIGCHLD-driven shutdown waits use child
notifications. Persistent detached logs use kqueue on macOS or inotify on
Linux, with bounded polling only as an unsupported-platform fallback.

Terminal focus and Meta key sequences preserve the draft; only a genuinely
bare Escape becomes a hard interrupt. Enter appends guidance to the active
turn's steering queue and notifies passive activity waits, which yield while
leaving their supervised work alive. Escape raises only the foreground abort
flag. While foreground commands are transferable, Ctrl+B moves the complete
active command batch into background supervision without restarting it. One
turn timestamp drives the fixed-rate 10 Hz status animation, which also reports
queued steering, live context, active background count, and applicable keyboard
hints. That row stays
directly above the input and changes in place instead of entering scrollback.
One capability policy filters both the exposed schema and executable registry,
including after MCP refresh. Global round/call limits remain safety ceilings;
tool-specific contracts such as visibility, call budgets, and stable arguments
constrain misuse without guessing task difficulty.
The MCP JSON-RPC boundary requires `2026-07-28` through `server/discover`.
Requests carry protocol, client identity, and client capabilities in `_meta`;
a server that does not advertise that version and tools capability is rejected
rather than downgraded. Server `roots/list` requests remain bounded by the
canonical workspace/global/per-server root policy.
Default lean delegated children do not clone the root session's MCP fleet;
request a full child when the delegated task actually needs MCP tools.

## Configuration snapshots

Bootstrap captures process overrides before importing config files, then builds
one immutable effective snapshot in precedence order. `/context` reports
redacted active/configured values, provenance for every `RuntimeConfig` field,
route details, and declared/observed provider capabilities. Secrets are shown
only as `<set>`/`<unset>` and URL userinfo is removed.

At each user or harness turn boundary, file stamps are checked synchronously.
A changed file is parsed into a fresh snapshot; request-, budget-, and
turn-scoped fields are atomically copied to both runtime config owners before
the turn starts. Startup-owned fields are reported as restart-required. There
is no watcher thread, no process-environment round trip, and no mid-turn
mutation.

## Context and cache

The stable prefix is system policy, project instructions, ordinary tool
schemas, and mostly append-only history. Dynamic environment metadata is added
only when it changes. Older completed tool outputs are replaced in meaningful
batches after two newer user turns and a recent-output budget; durable tool
outputs opt out through registry metadata. A byte-identical repeated source
read gets a short receipt only while its original result remains in that recent
window. At projected 85% context pressure, including pending input and schemas,
one tool-free model call summarizes a 256 KiB semantic head/tail projection—
user and assistant prose plus summarized calls and bounded results—rather than
bulky tool-protocol envelopes. History changes only after validation. Automatic
pre-turn and at-most-once mid-turn compaction use the `/compact` path. The
replacement context keeps recent real user messages independently of the
model-generated summary (up to 80 KiB, reduced for small context windows),
keeps a prior summary at prose rather than tool-evidence size, and reinjects
current runtime context before the retained goal. This makes continuation less
dependent on a perfect summary while staying well below the next compaction
threshold.

Official OpenAI Responses requests add a hashed, session-stable
`prompt_cache_key`; compatible/custom endpoints receive no OpenAI-only field.
Anthropic Messages requests use its top-level moving `cache_control` breakpoint
for growing conversations. Each provider still determines cache eligibility,
while reported cache-read and cache-write tokens remain the evidence used for
performance comparisons.

Structured context-overflow codes, proxy-wrapped canonical codes, and HTTP 413
form a separate non-retryable class. With no streamed content, usage,
annotations, or calls, µAgent learns a conservative route bound, compacts, and
retries the original turn once. Unsafe or repeated overflow stops the turn.

When enabled, `adapt_system` owns one separately persisted free-form directive.
The tool may replace or clear it at any step; the agent then reconstructs
message zero from the immutable core plus the latest revision before the next
request. Complete replacement avoids accumulating stale prompt fragments.
The model-facing contract treats revision as an exceptional response to a
concrete observation and requires the reason to identify the corresponding
strategy delta, discouraging an automatic generic rewrite at turn start.
Host code still owns permissions, approvals, capabilities, and limits. Debug
telemetry records each revision and a full snapshot of the next request.

Active messages and the removed-trace archive remain separate. Automatic
memory extraction writes a per-activity private receipt and a bounded private
`memory/events.jsonl` audit. The parent displays created/updated/failure
receipts as maintenance UI, never as model context or an automatic continuation.
Successful archived `show_image` calls are matched to their tool results
by call ID and retransmitted at the original timeline position. Sessions retain
the path and tool trace, not image bytes; unavailable paths fail safely.
Model-authored summary and memory text is evidence, never user authority. Native, Codex, and
current-project Claude names share one bounded index; external files are
read-only. The explicit `memory(action, key, content?)` contract handles get,
set, forget, list, and search. Foreground writes require an explicit user
request. One supervised startup child may extract one native memory from one
idle saved session; it receives a redacted 32 KiB transcript and only the
memory tool. `--no-memory` disables recall and extraction. Required behavior
belongs in `AGENTS.md`, not memory.
Skills use progressive disclosure: a bounded catalogue of installed names and
descriptions is advertised with the `skill` tool. Explicit `$skill-name`
mentions are resolved before the first model call; otherwise the model selects
through the tool. Discovery includes user roots plus ancestor `.agents/skills`
and nested skills to depth six. A selected `SKILL.md` is loaded completely or
rejected as oversized—partial procedures are never injected. Tool requirements
filter unusable skills before advertisement; invocation arguments expand only
when the selected body is loaded.
Browser work follows the same pattern: the deferred `browser-use` skill drives
`playwright-cli` through the existing approved `run` tool. Playwright's daemon
owns browser state; µAgent adds no browser schemas or protocol layer.

The complete built-in, conditional, and dynamically discovered tool inventory
is documented in [TOOLS.md](TOOLS.md). The active registry remains authoritative
and is visible through `/context`.

## Observability

`EventId` and one compile-time policy table define stable application,
debug/public names, durability, and public projection. Terminal, JSONL, debug,
and journal sinks are concrete direct owners. Additional in-process adapters
subscribe through `Observability::Subscribe`; each `AppEvent` has a monotonic
sequence, timestamp, stable dotted type, structured data, and durability bit.
Callbacks run outside the observability lock and cannot steer agent control
flow. Transient reasoning/answer deltas are rendered but never journaled. With
`UAGENT_HEADLESS_PROGRESS=1` — set for a background child, whose stderr already
lands in the log its parent polls — the same durable events are echoed as one
stderr line each, so a delegated run is traceable while it works; the stdout
answer contract is unchanged. Turn, tool,
capability, config, notice, and session lifecycle events append bounded
metadata to a private sidecar journal without entering model context. A
`session.ready` provenance object uses the generated prompt/surface identities
plus allowlisted behavior settings, so offline reports can cohort builds without
paths, hosts, prompts or secrets. Structured argument issues, activity
no-change/terminal facts, turn resources and usage stay factual in the journal;
recovery and repetition labels are derived offline.

User-facing notices — interruptions, budget failures, compaction, degraded
capability — are events, not prints. The agent loop owns no terminal: severity
selects a color in the presenter and nothing else, so the same notice reaches
the journal and the JSONL whether or not a terminal is attached.

The input half is `ApplicationChannel`: an adapter supplies prompts and slash
commands and answers typed interaction requests while consuming the event
stream. Model and session pickers carry their complete option sets in
`interaction.requested`; the reply uses the same correlation ID. Approval
requests and resolutions are explicit events, including automatic and
mandatory-human decisions. Every slash command finishes with semantic result
data plus the complete interface state in `command.completed`; direct terminal
formatting is suppressed when a channel owns the session. A stdio adapter keeps
its protocol descriptor independently before bootstrap silences process
stdout. This is the
transport-neutral seam for a future Codex-style app-server: JSON-RPC over stdio
or WebSocket belongs in a small adapter around the channel and subscription,
not in the agent loop.

The API stream layer only decodes and assembles provider traffic. A terminal
presenter owns Markdown, composer interaction, compact/verbose reasoning, and
ANSI restoration. Compact reasoning takes the latest line, removes generic
lightweight decoration, collapses whitespace, and retains the latest complete
words without a synthetic leading ellipsis; it has no provider/model syntax
branches. Tool registry metadata produces provider-independent presentation
records shared by live output, history, `/trace`, debug, and journal records.
Parallel results are observed in completion order while protocol messages stay
in call order. Background completion is observational: command output updates
UI and retained activity state but never starts or enters a model turn. A failed
delegated-child row uses a bounded category-only stage/reason; the complete
route, remedy, diagnostics and artifact stay in the retained result. Bounded
task completion is added once to the next naturally occurring model call
without triggering one; multiple task completions share a 12 KiB message. Explicit `activity`
can replay a retained bounded transcript.

Interactive status exposes model/effort, endpoint, context, cache, cost,
background count, queue depth, working time, and transferable foreground work.
Reasoning is collected in every mode. Provider replay blocks are retained on
assistant messages according to emitted fields and route capability, while
`--debug` captures complete flattened reasoning and structured details.
Verbose mode sends a labelled, muted stream into scrollback alongside expanded
bounded tool output.

`--debug` uses an ordered background writer; deterministic shutdown drains and
joins it before process exit. `--json-stream` is versioned as `uagent.event.v2`;
`--json` keeps its separate result envelope. No OTLP SDK is linked: the bounded
JSONL formats are the optional telemetry boundary, and an external collector
can tail them if deployment needs justify it.

## Failure model

- Retry transient transport failures only before semantic progress.
- Degrade unsupported optional request features once and record it.
- Isolate optional MCP failure to one server; fail bootstrap when a required
  server cannot start, initialize, or list its tools.
- Cancel and reap owned process groups on catchable exits.
- Keep debug and persisted state private and bounded.

## Extending

Add tools through `MakeTool`, route behavior through
`ProviderCapabilities`, turn-static limits through `RuntimeConfig`, and state
through a versioned atomic store. Tool behavior that affects scheduling or
presentation belongs in tool metadata, not string comparisons against tool
names. Live environment access is limited to intentionally dynamic route and
delegation state. Every new boundary needs a focused unit test; externally visible behavior
needs a hermetic integration test. Model behavior—batching, deduplication,
round count, and answer shape—gets a scored scenario in
`benchmarks/scenarios/` and comparison against a committed baseline. See
[CONTRIBUTING.md](../CONTRIBUTING.md) and [TESTING.md](TESTING.md).
