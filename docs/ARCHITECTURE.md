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
| `include/mcp/` | bounded stdio JSON-RPC and Chrome integration |
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

Time, rounds, calls, output, processes, memory, context, and reported cost are
bounded. Persistent commands require `run(detach=true)`. Delegated work runs in
separate sanitized processes. One `ProcessSupervisor` owns commands, tasks,
and detached services under activity IDs. `activity_output` snapshots a bounded
log without changing ownership. A task runs in the foreground when its result is
required for the next step, or in the background when useful parent work can
continue. Background results are delivered automatically on exit;
`activity_wait` is an optional join for blocked work, and `activity_stop`
terminates the complete process group. An optional output wait watches for new
bytes, exit, or a readiness marker instead of repeated polling. Results arrive
at model-step boundaries. The persistent interactive loop and headless runner
both drain completions and resume through an internal harness turn that does not
count as user input. They reuse the existing event poll and process supervisor,
without a watcher thread or parallel registry. Detached ownership follows the
process group after its wrapper exits.
The application owns a small raw-mode composer while the foreground agent runs
on one worker thread. Agent output is marshalled back above the two-line
composer, preserving native scrollback. A stateful decoder retains fragmented
CSI and bracketed-paste sequences across reads; the composer owns history and
paste bounds.
Terminal focus and Meta key sequences preserve the draft; only a genuinely
bare Escape becomes an interrupt. Enter appends guidance to the active turn's steering
queue; Escape raises only the foreground abort flag. One turn
timestamp drives the sole dynamic status row, which also reports queued
steering, live context, and active background count. That row stays directly
above the input and changes in place instead of entering scrollback.
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

Active messages and the removed-trace archive remain separate. Model-authored
summary and memory text is evidence, never user authority. Memory bodies are
read on demand from a bounded name-only index. The explicit
`memory(action, key, content?)` contract handles get, set, forget, list, and
search; writes require an explicit user request. `--no-memory` removes recall
and writes for reproducible runs. Required behavior belongs in `AGENTS.md`, not
memory.
Skills use progressive disclosure: a bounded catalogue of installed names and
descriptions is advertised with the `skill` tool. Explicit `$skill-name`
mentions are resolved before the first model call; otherwise the model selects
through the tool. Discovery includes user roots plus ancestor `.agents/skills`
and nested skills to depth six. A selected `SKILL.md` is loaded completely or
rejected as oversized—partial procedures are never injected. Tool requirements
filter unusable skills before advertisement; invocation arguments expand only
when the selected body is loaded.
## Observability

Interactive status exposes model/effort, endpoint, context, cache, cost,
background count, queue depth, and working time.
`--debug` records reconstructable JSONL; `--json` and `--json-stream` expose
stable automation formats.

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
