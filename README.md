# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

µAgent is a local coding-agent harness shipped as one native binary, without a
language runtime, application framework, or plugin system. Explicit route
adapters stream Chat Completions, OpenAI Responses, and Anthropic Messages. A
single process supervisor owns commands and resumable collaborators; typed
events provide inspectable evidence without driving control flow.

A release build is about a 2 MB executable linking `libcurl`, `libc++`, and
`libSystem`. Its only vendored source dependency is `json.hpp`. Linux and macOS
are supported.

## Why µAgent

**No runtime to maintain.** Most coding agents depend on Node or Python,
adding a package manager, dependency tree, and runtime. µAgent is a C++20
binary built with CMake and `-fno-exceptions`. Optional tools use `uv` or
Playwright only when invoked.

**Provider neutrality is an invariant, not a setting.** A canonical
conversation and tool protocol feed explicit Chat Completions, Responses, and
Anthropic adapters. Route capabilities declare transport and hosted tools;
model names never grant capabilities. Configured route metadata and the exact
official OpenAI endpoint select wire-dialect details; reasoning replay,
citations, usage, retries, and rendering remain shared.

**Every limit is explicit and inspectable.** Requests, idle streams, tool
output, processes, memory, and context are bounded by default. Aggregate model
rounds, tool calls, generated tokens, turn time, and provider-reported spend
have opt-in caps.
Settings resolve from files, environment, and flags with visible provenance,
validate against declared bounds, and reload only between turns.
`/status` summarizes version, route, effort, approval mode, and budgets;
`/debug-config` explains active values; `/context` shows effective configuration,
provenance, conversation state, and currently advertised tool schemas. Steering,
activity completion, attachment draining, and compaction may still change the
next wire request at its normal turn boundary.

**Processes are first-class, not fire-and-forget.** Commands run under a real
supervisor with opaque activity IDs: optional PTYs, writable stdin, resize,
wait, stop, background handoff mid-run with Ctrl+B, and log-only detach that
deliberately outlives the µAgent session. Session-scoped children are cleaned
up on exit; detached services remain discoverable and stoppable later.

**One observational spine, deliberately not a plugin system.** Semantic events
are projected to fixed consumers — terminal presentation, versioned
`uagent.event.v2` JSONL, a sensitive debug trace, and a bounded metadata-only
session journal — according to each consumer's contract. Emitters cannot read
sink state or receive a result. In-process subscribers cannot steer control
flow, and there is no plugin-loaded sink or OpenTelemetry dependency; consume
the JSONL externally instead.

That set of choices makes µAgent useful for studying model behavior — tool use,
provider quirks, degradation, and cost — because the evidence and authority
boundaries are explicit. The executable and dependency surface are small; the
internal turn, process, persistence, and protocol machinery are substantial and
kept as explicit domains rather than hidden behind a framework.

## Quick start

Requires CMake, a C++20 compiler, and libcurl. [uv](https://docs.astral.sh/uv/)
enables Python scratch scripts. Browser automation additionally requires
Node.js 20+ and `npm install -g @playwright/cli@latest`.

```sh
./install.sh
```

The installer uses `build/release` and reuses an existing preset build instead
of compiling the binary a second time.

Create `~/.uagent/.config`:

```dotenv
OPENROUTER_API_KEY=replace-me
OPENROUTER_MODEL=deepseek/deepseek-v4-flash
```

Any OpenAI-compatible endpoint can instead use `UAGENT_BASE_URL`,
`UAGENT_API_KEY`, and `UAGENT_MODEL`. Named routes use `UAGENT_PROVIDERS`.
Environment values override trusted project and user configuration; project
`.env` files are never loaded. The bundled `$uagent-config` skill contains the
complete configuration reference.

```sh
uagent
uagent -p "inspect this repository"
uagent -p "inspect this repository" --json
uagent -p "inspect this repository" --json-stream --budget 2 --token-budget 20000
uagent -c
uagent --resume
uagent --debug=/tmp/uagent.jsonl
uagent --yolo
```

## Highlights

- Native streamed answers and reasoning across OpenAI-compatible
  `reasoning`, `reasoning_details`, and `reasoning_content` fields; compact
  mode keeps a clean latest-line preview in the transient status row without a
  leading ellipsis, while `/verbose` preserves the labelled full stream.
- Persistent editable composer with queued steering, Escape interruption, and
  Ctrl+B foreground-command handoff.
- Parallel safe tools, durable resumable collaborators, automatic compaction,
  and resumable workspace sessions.
- Supervised process activities with opaque IDs, incremental output, optional
  PTYs, writable input, resize, wait, stop, and persistent log-only detach.
- Repository tools, attachments, terminal images, web search, skills, memory,
  Playwright automation, and dynamically discovered MCP tools. MCP stdio
  requires the stateless `2026-07-28` protocol; there is no legacy downgrade.
- One typed application event spine with terminal, stable JSONL, sensitive
  debug, bounded session-journal, and in-process subscriber consumers, plus a
  transport-neutral input channel for future GUI/app-server adapters.
- Centralized route capabilities, provider-independent tool presentation, and
  explicit Chat Completions, Responses, and Anthropic Messages adapters.
- Native hosted web search when the active route declares it, otherwise an
  explicitly configured OpenRouter search route; model names never imply
  support.
- Redacted effective configuration and provenance in `/context`, with validated
  request/turn settings reloaded only between turns.
- Semantic context-overflow recovery: one bounded compaction and at most one
  safe retry. Command completion stays in UI/retained activity state; bounded
  subagent completion joins the next naturally occurring model call without
  starting one.
- Explicit limits for time, output, processes, context, persistence, and
  provider-reported spend.

## Tools

The core registry includes:

| Area | Tools |
| --- | --- |
| inspect | `read_path`, `grep` |
| mutate | `write_file`, `edit_file`, `delete_file` |
| execute | `run`, `scratch` |
| activities | `activity` |
| evidence and state | `attach`, `show_image`, `memory`, `uagent_info`, `uagent_configure` |
| web | `web_fetch` |
| conditional | `web_search`, `subagent`, `skill`, `adapt_system`, MCP tools |

Policy, lean mode, route capabilities, runtime state, and configuration filter
the active schemas; see [the tool reference](docs/TOOLS.md). Compact reasoning
updates only the transient activity row, while `/verbose` restores full muted
reasoning and expanded bounded tool output.

## Interactive controls

| Input | Action |
| --- | --- |
| Enter while working | Queue steering; passive activity waits yield at once |
| Ctrl+B during a command | Move the foreground command batch to background supervision |
| Escape | Interrupt the foreground operation and apply queued steering |
| Shift+Enter, Alt+Enter | Keep the draft open on a new line; Enter still submits |
| Tab after `/` | Complete the command being typed from the rows below the draft |
| Tab after `@` | Complete a path one segment at a time from the same rows |
| Ctrl+X Ctrl+E | Open the draft in `$VISUAL`/`$EDITOR` and take back what it saves |
| Ctrl+C while idle | Ask first; a second press within two seconds exits |
| `/models`, `/model` | Search or change model route |
| `/effort`, `/variant` | Change reasoning effort or OpenRouter routing |
| `/attach` | Queue or clear an attachment |
| `/context`, `/trace`, `/cost`, `/ps`, `/agents`, `/tools` | Inspect active state |
| `/compact`, `/sessions`, `/reset` | Manage context and sessions |
| `/init`, `/review`, `/diff` | Write AGENTS.md, review changes, or show the git diff |
| `/memory` | Show saved memory action, time, source, and redacted preview |
| `/verbose` | Toggle full reasoning and expanded bounded tool output |
| `/yolo` | Toggle automatic approval |
| `/help`, `/quit` | Show help or exit |

Approval prompts accept `y` once or `a` for the session. Shell approval reuse is
scoped to the exact command payload; approving one `git`, shell, or interpreter
invocation cannot authorize another. Any other answer denies the call and sends
the denial to the model as steering.

Successful historical `show_image` calls are retransmitted at their original
position when a session resumes, provided the recorded local file still exists.
Image bytes are not embedded in the session.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Operations and limits](docs/OPERATIONS.md)
- [Tools](docs/TOOLS.md)
- [Persistence](docs/PERSISTENCE.md)
- [Testing](docs/TESTING.md)
- [Bounded self-improvement](docs/SELF_IMPROVEMENT.md)
- [Security](SECURITY.md)
- [Contributing](CONTRIBUTING.md)

## Development

```sh
uv sync --frozen
uv run --frozen ruff check tests
uv run --frozen ruff format --check tests
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

First-party C++ follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
µAgent is a local single-user CLI. It confines the commands it runs with the
OS sandbox — writes only, on by default, `UAGENT_SANDBOX=0` to opt out — but it
is not a container; use a restricted account, container, or VM for untrusted
code.
