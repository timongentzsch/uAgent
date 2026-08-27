# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

µAgent is a coding agent shipped as one native binary, without a language
runtime, application framework, or plugin system. Explicit route adapters
stream Chat Completions, OpenAI Responses, and Anthropic Messages. The agent
supervises its child processes and emits typed events for logging, replay, and
assertions.

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
model names and endpoint substrings never grant capabilities. Reasoning replay,
citations, usage, retries, and rendering remain shared.

**Every limit is explicit, bounded, and inspectable.** Requests, idle streams,
tool output, processes, memory, context, and reported spend are bounded by
default. Settings resolve from files, environment, and flags with visible
provenance, validate against declared bounds, and reload only between turns.
`/status` summarizes version, route, effort, approval mode, and budgets;
`/debug-config` explains active values; `/context` shows effective configuration,
provenance, and the exact next request.

**Processes are first-class, not fire-and-forget.** Commands run under a real
supervisor with opaque activity IDs: optional PTYs, writable stdin, resize,
wait, stop, background handoff mid-run with Ctrl+B, and log-only detach that
outlives the turn. Nothing leaks on exit.

**One observational spine, deliberately not a plugin system.** Every semantic
event fans out to four fixed sinks — terminal, versioned `uagent.event.v2`
JSONL, a sensitive debug trace, and a bounded metadata-only session journal.
Emitters cannot read sink state or receive a result, so telemetry and rendering
can never steer agent control flow. There is no runtime sink registration and
no OpenTelemetry dependency; consume the JSONL externally instead.

That set of choices makes µAgent a good harness for studying how models
actually behave — tool use, provider quirks, degradation, cost — because the
evidence is structured and the machinery between you and the model is thin.

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
uagent -p "inspect this repository" --json-stream --budget 2
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
- Parallel safe tools, bounded delegated tasks, automatic compaction, and
  resumable workspace sessions.
- Supervised process activities with opaque IDs, incremental output, optional
  PTYs, writable input, resize, wait, stop, and persistent log-only detach.
- Repository tools, attachments, terminal images, web search, skills, memory,
  Playwright automation, and dynamically discovered MCP tools.
- One typed observational event spine with fixed terminal, stable JSONL,
  sensitive debug, and bounded metadata-only session-journal consumers.
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
| mutate | `write_file`, `edit_file` |
| execute | `run`, `scratch` |
| activities | `activity` |
| evidence and state | `attach`, `show_image`, `memory` |
| web | `web_fetch` |
| conditional | `web_search`, `subagent`, `skill`, `adapt_system`, MCP tools |

Policy, lean mode, route capabilities, runtime state, and configuration filter
the active schemas. Compact reasoning updates only the transient activity row;
tool calls remain fully visible. `/verbose` restores full muted reasoning and
expanded bounded tool output. `/context` reports redacted configuration,
provenance, restart-required fields, route capabilities, and the exact next
request. See [the tool reference](docs/TOOLS.md).

## Interactive controls

| Input | Action |
| --- | --- |
| Enter while working | Queue steering; passive activity waits yield at once |
| Ctrl+B during a command | Move the foreground command batch to background supervision |
| Escape | Interrupt the foreground operation and apply queued steering |
| Shift+Enter, Alt+Enter | Keep the draft open on a new line; Enter still submits |
| Tab after `/` | Complete the command being typed from the rows below the draft |
| Ctrl+C while idle | Ask first; a second press within two seconds exits |
| `/models`, `/model` | Search or change model route |
| `/effort`, `/variant` | Change reasoning effort or OpenRouter routing |
| `/attach` | Queue or clear an attachment |
| `/context`, `/trace`, `/cost`, `/ps`, `/tools` | Inspect active state |
| `/compact`, `/sessions`, `/reset` | Manage context and sessions |
| `/init`, `/review`, `/diff` | Write AGENTS.md, review changes, or show the git diff |
| `/memory` | Show saved memory action, time, source, and redacted preview |
| `/verbose` | Toggle full reasoning and expanded bounded tool output |
| `/yolo` | Toggle automatic approval |
| `/help`, `/quit` | Show help or exit |

Approval prompts accept `y` once or `a` for that tool—and, for shell calls, the
command's first word—for the rest of the session. Any other answer denies the
call and sends the denial to the model as steering.

Successful historical `show_image` calls are retransmitted at their original
position when a session resumes, provided the recorded local file still exists.
Image bytes are not embedded in the session.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Operations and limits](docs/OPERATIONS.md)
- [Tools](docs/TOOLS.md)
- [Persistence](docs/PERSISTENCE.md)
- [Testing](docs/TESTING.md)
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
µAgent is a local single-user CLI, not an OS sandbox; use a restricted account,
container, or VM for untrusted code.
