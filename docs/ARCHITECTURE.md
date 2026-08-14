# Architecture

µAgent is one C++20 coordinator binary with explicit resource owners, owned
child processes, and no runtime framework.

```text
main → Bootstrap → Application
                  ├─ Api + HTTP/SSE
                  ├─ Agent + Conversation
                  ├─ Tool registry + ProcessSupervisor
                  ├─ MCP runtime
                  └─ Usage + session storage
```

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

Every route mutation uses one activation path: reset discovered capabilities,
export child state, then rotate the agent route identity. Provider protocol is
explicit for custom proxies.

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

Time, output, processes, memory, and context are bounded by default; model
rounds, tool calls, and reported cost have configurable opt-in caps. Persistent commands require `run(detach=true)`. Delegated work runs in
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

A single process-I/O thread polls PTY masters and ordinary command pipes. It
feeds the private log, an incremental 1 MiB head/tail buffer, and an aggregate
bounded transcript, and it is the sole owner of process reaping and I/O-FD
closure. The buffer preserves the oldest and newest bytes
and reports an omitted middle. `activity_output` drains new output or waits for
output, exit, or a readiness marker; `activity_input` serializes PTY writes,
polling, interruption, and resize for one activity. `activity_wait` optionally
joins blocked work, while `activity_stop` terminates the complete process group.
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
Terminal focus and Meta key sequences preserve the draft; only a genuinely
bare Escape becomes an interrupt. Enter appends guidance to the active turn's
steering queue; Escape raises only the foreground abort flag. While foreground
commands are transferable, Ctrl+B moves the complete active command batch into
background supervision without restarting it. One turn timestamp drives the
fixed-rate 10 Hz status animation, which also reports queued steering, live
context, active background count, and applicable keyboard hints. That row stays
directly above the input and changes in place instead of entering scrollback.
One capability policy filters both the exposed schema and executable registry,
including after MCP refresh. Global round/call limits remain safety ceilings;
tool-specific contracts such as visibility, call budgets, and stable arguments
constrain misuse without guessing task difficulty.
The MCP JSON-RPC boundary also handles server-initiated requests. Every session
advertises a canonical root set and answers `roots/list`; roots are
resolved once from workspace/global/per-server policy and survive lazy starts
and restarts with the server configuration.

## Context and cache

The stable prefix is system policy, project instructions, ordinary tool
schemas, and mostly append-only history. Dynamic environment metadata is added
only when it changes. Older completed tool outputs are replaced in meaningful
batches after two newer user turns and a recent-output budget; durable tool
outputs opt out through registry metadata. A byte-identical repeated source
read gets a short receipt only while its original result remains in that recent
window. At projected 85% context pressure, including pending input and schemas,
one tool-free model call creates a bounded summary. History changes only after that
summary passes validation. Automatic pre-turn and at-most-once mid-turn
compaction use the same path as `/compact`.

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

Active messages and the removed-trace archive remain separate. On conversation
resume, successful archived `show_image` calls are matched to their tool results
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

Interactive status exposes model/effort, endpoint, context, cache, cost,
background count, queue depth, working time, and transferable foreground work.
`--debug` records reconstructable JSONL through an ordered background writer so
serialization and flushing stay off the request path. Model-response records
separate request preparation and end-to-end time from DNS, connection, TLS,
pre-transfer, start-transfer, first-semantic-event, and total request timings.
`--json` and `--json-stream` expose stable automation formats.

## Failure model

- Retry transient transport failures only before semantic progress.
- Degrade unsupported optional request features once and record it.
- Isolate MCP failure to one server.
- Cancel and reap owned process groups on catchable exits.
- Keep debug and persisted state private and bounded.

## Extending

Add tools through `MakeTool`, provider quirks at request normalization,
session-static limits through `RuntimeConfig`, and state through a versioned
atomic store. Live environment access is limited to intentionally dynamic
route and delegation state. Every new
boundary needs a focused unit test and externally visible behavior needs a
hermetic integration test. See [CONTRIBUTING.md](../CONTRIBUTING.md) and
[TESTING.md](TESTING.md).
