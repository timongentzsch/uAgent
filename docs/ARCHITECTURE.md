# Architecture

µAgent is one C++20 process with explicit resource owners and no runtime
framework.

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
| `include/providers.h` | provider setup, model routes, certified profiles |
| `include/tools/` | bounded capabilities and process ownership |
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

## Turn

```text
user input
  → project instructions and deferred memory index
  → context-pressure decision
  → streamed model response
  → validate and approve tool calls
  → run independent safe calls concurrently, stateful calls serially
  → append bounded results in call order
  → repeat until final text
  → archive trace and atomically save session
```

Time, rounds, calls, output, processes, memory, context, and reported cost are
bounded. Persistent commands require `run(detach=true)`. Delegated work runs in
separate sanitized processes and joins before the next model request.

## Context and cache

The stable prefix is system policy, project instructions, ordinary tool
schemas, and append-only history. Dynamic environment metadata is added only
when it changes. At context pressure the model may produce a bounded,
non-authoritative checkpoint; applying it intentionally starts a fresh cache
prefix. `/handoff` uses that boundary to change routes.

Active messages, removed-trace archive, and checkpoints remain separate.
Model-authored memory or checkpoint text is evidence, never user authority.
See [CHECKPOINTS.md](CHECKPOINTS.md).

## Observability and evaluation

Interactive status exposes route, context, tokens, cache, cost, and timing.
`--debug` records reconstructable JSONL; `--json` and `--json-stream` expose
stable automation formats. `uagent eval` runs isolated scenarios and writes
fresh route profiles. Profiles may only narrow advertised capabilities and
expire after 30 days.

## Failure model

- Retry transient transport failures only before semantic progress.
- Degrade unsupported optional request features once and record it.
- Treat certified-profile mismatches as stale evidence, then self-heal.
- Isolate MCP failure to one server.
- Cancel and reap owned process groups on catchable exits.
- Keep debug and persisted state private and bounded.

## Extending

Add tools through `MakeTool`, provider quirks at request normalization, limits
through `RuntimeConfig`, and state through a versioned atomic store. Every new
boundary needs a focused unit test and externally visible behavior needs a
hermetic integration test. See [CONTRIBUTING.md](../CONTRIBUTING.md) and
[TESTING.md](TESTING.md).
