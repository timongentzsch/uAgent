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
                       ├─ uagent.event.v1 JSONL
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
| `src/agent/`, `include/agent/` | conversation, compaction, tool loop |
| `src/api/`, `include/api/` | OpenAI-compatible requests and streaming |
| `src/providers.cc`, `include/providers.h` | provider catalogue and route activation |
| `src/tools/`, `include/tools/` | bounded capabilities and process ownership |
| `include/mcp/` | bounded stdio JSON-RPC integrations |
| `include/core/` | configuration, usage, diagnostics, platform helpers |
| `include/ui/`, `include/cli.h` | inline terminal rendering and input |

`Conversation` owns model-visible messages and the bounded archive. `Agent`
compares estimated/reported context against one threshold; providers and tools
do not own conversation state. `ProcessSupervisor`,
`McpRuntime`, and `UsageAccumulator` each own one class of external resource.

Shared policy stays centralized: `MakeTool` defines tool metadata,
`RuntimeConfig` defines limits, `RouteKey` defines route identity, and
`HeadlessResult` defines machine output.

Web search follows the same boundary: models always see one named function,
while its host adapter selects the provider protocol and owns limits, citations,
errors, and usage accounting.

Every route mutation uses one activation path: construct a centralized
`ProviderCapabilities` contract, export its stable child-process projection,
then rotate the agent route identity. Request serialization, model catalogues,
reasoning replay, search protocol, and negotiated degradation read that
contract rather than provider/model names. Successful responses add observed
reasoning, citation, and usage facts without controlling the current turn.

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
opaque IDs distinct from OS PIDs; persistent detached records remain PID-backed
for reattachment compatibility.

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
timer; only the bounded trailing-output grace creates a real deadline. The thread feeds the private log, an incremental 1 MiB head/tail
buffer, and an aggregate bounded transcript, and it is the sole owner of
process reaping and I/O-FD closure. The buffer preserves the oldest and newest bytes
and reports an omitted middle. `activity` drains new output or waits for
output, exit, a readiness marker, or queued steering; writing chars
serializes PTY writes, polling, interruption, and resize for one activity.
an id-less wait optionally joins blocked work and yields on queued steering
without cancelling it, while `activity_stop` terminates the complete process
group.
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
The MCP JSON-RPC boundary also handles server-initiated requests. Every session
advertises a canonical root set and answers `roots/list`; roots are
resolved once from workspace/global/per-server policy and survive lazy starts
and restarts with the server configuration.

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
pre-turn and at-most-once mid-turn compaction use the `/compact` path.

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
Host code continues to own permissions, approvals, capabilities, and limits.
Debug telemetry records revisions and forces a full request snapshot after an
in-place system-message change.

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

`EventId` and one compile-time policy table define stable debug/public names,
durability, and public projection. Terminal, JSONL, debug, and journal sinks
are concrete direct owners; there is no runtime sink registration. Transient
reasoning/answer deltas are rendered but never journaled. Turn, tool,
capability, config, and session lifecycle events append bounded metadata to a
private sidecar journal without entering model context.

The API stream layer only decodes and assembles provider traffic. A terminal
presenter owns Markdown, composer interaction, compact/verbose reasoning, and
ANSI restoration. Compact reasoning takes the latest line, removes generic
lightweight decoration, collapses whitespace, and retains the latest complete
words without a synthetic leading ellipsis; it has no provider/model syntax
branches. Tool registry metadata produces provider-independent presentation
records shared by live output, history, `/trace`, debug, and journal records.
Parallel results are observed in completion order while protocol messages stay
in call order. Background completion is observational: command output updates
UI and retained activity state but never starts or enters a model turn. Bounded
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
joins it before process exit. `--json` and the existing `uagent.event.v1`
`--json-stream` schema remain stable. No OTLP SDK is linked: the bounded JSONL
formats are the optional telemetry boundary, and an external collector can
tail them if deployment needs justify it.

## Failure model

- Retry transient transport failures only before semantic progress.
- Degrade unsupported optional request features once and record it.
- Isolate MCP failure to one server.
- Cancel and reap owned process groups on catchable exits.
- Keep debug and persisted state private and bounded.

## Extending

Add tools through `MakeTool`, route behavior through
`ProviderCapabilities`, turn-static limits through `RuntimeConfig`, and state
through a versioned atomic store. Tool behavior that affects scheduling or
presentation belongs in tool metadata, not string comparisons against tool
names. Live environment access is limited to intentionally dynamic route and
delegation state. Every new
boundary needs a focused unit test and externally visible behavior needs a
hermetic integration test. See [CONTRIBUTING.md](../CONTRIBUTING.md) and
[TESTING.md](TESTING.md).
