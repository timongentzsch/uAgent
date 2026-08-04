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
| `src/agent/`, `include/agent/` | conversation, context, checkpoints, tool loop |
| `src/api/`, `include/api/` | OpenAI-compatible requests and streaming |
| `src/providers.cc`, `include/providers.h` | route activation and profiles |
| `src/tools/`, `include/tools/` | bounded capabilities and process ownership |
| `include/mcp/` | bounded stdio JSON-RPC and Chrome integration |
| `include/core/` | configuration, usage, diagnostics, platform helpers |
| `include/ui/`, `include/cli.h` | inline terminal rendering and input |

`Conversation` owns model-visible messages and the bounded archive.
`ContextPolicy` alone decides context pressure. `Agent` coordinates both;
providers and tools do not own conversation state. `ProcessSupervisor`,
`McpRuntime`, and `UsageAccumulator` each own one class of external resource.

Shared policy stays centralized: `MakeTool` defines tool metadata,
`RuntimeConfig` defines limits, `RouteKey` defines route identity, and
`HeadlessResult` defines machine output.

Every route mutation uses one activation path: restore configured policy,
reset capabilities, apply certified evidence, export child state, then rotate
the agent route identity. Provider protocol is explicit for custom proxies.

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
separate sanitized processes; results arrive at step boundaries, while
`wait_agent` pauses only when the model needs another result.
`terminal_output` snapshots the bounded log of any supervised background
process without joining, reaping, or changing its ownership.
The application owns a small raw-mode composer while the foreground agent runs
on one worker thread. Agent output is marshalled back above the two-line
composer, preserving native scrollback. Enter appends guidance to the active
turn's steering queue; Escape raises only the foreground abort flag. One turn
timestamp drives the sole dynamic status row, which also reports queued
steering and active background count. That row stays directly above the input
and changes in place instead of entering scrollback.
One capability policy filters both the exposed schema and executable registry,
including after MCP refresh. Global round/call limits remain safety ceilings;
tool-specific contracts such as visibility, call budgets, and stable arguments
constrain misuse without guessing task difficulty.

## Context and cache

The stable prefix is system policy, project instructions, ordinary tool
schemas, and mostly append-only history. Dynamic environment metadata is added
only when it changes. Older completed tool outputs are replaced in meaningful
batches after two newer user turns and a recent-output budget; durable tool
outputs opt out through registry metadata. A byte-identical repeated source
read gets a short receipt only while its original result remains in that recent
window. At context pressure the model may produce a bounded, non-authoritative
checkpoint; applying it intentionally starts a fresh cache prefix. `/handoff`
uses that boundary to change routes.

Active messages, removed-trace archive, and checkpoints remain separate.
Model-authored memory or checkpoint text is evidence, never user authority.
Memory bodies are read on demand from a bounded name-only index. The explicit
`memory(action, key, content?)` contract handles get, set, and forget. Explicit
user requests may write immediately. Automatic contribution is delayed: one
eligible prior session per interactive startup is claimed after its idle
window, stripped of system/runtime/reasoning/image content, secret-redacted,
and sent to a bounded child with only the memory tool. That child consolidates
at most one durable preference, workflow, constraint, or debugging insight
into the existing scoped files, or does nothing. Its maintenance result never
enters the active conversation, foreground steering does not cancel it, and a
resumed session becomes eligible again after it changes and goes idle. This
keeps the important extraction/consolidation lifecycle without a second
database or memory store. Recall and contribution are separate controls;
`--no-memory` removes both for reproducible runs. Required behavior belongs in
`AGENTS.md`, not memory.
Skills use progressive disclosure: a bounded catalogue of installed names and
descriptions is advertised with the `skill` tool. Explicit `$skill-name`
mentions are resolved before the first model call; otherwise the model selects
through the tool. Discovery includes user roots plus ancestor `.agents/skills`
and nested skills to depth six. A selected `SKILL.md` is loaded completely or
rejected as oversized—partial procedures are never injected.
See [CHECKPOINTS.md](CHECKPOINTS.md).

## Observability and evaluation

Interactive status exposes model/effort, endpoint, context, cache, cost,
background count, queue depth, and working time.
`--debug` records reconstructable JSONL; `--json` and `--json-stream` expose
stable automation formats. `uagent eval` runs isolated scenarios; explicit
certification records the exact passing suite. Only identical scenario sample
sets select the cheapest compliant effort. Capability contradictions
persistently invalidate profiles, which expire after 30 days.

## Failure model

- Retry transient transport failures only before semantic progress.
- Degrade unsupported optional request features once and record it.
- Treat certified-profile mismatches as stale evidence, then self-heal.
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
