# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

µAgent is a local coding-agent harness shipped as one native binary, without a
server-side language runtime or plugin system. Explicit route
adapters stream Chat Completions, OpenAI Responses, and Anthropic Messages. A
conversation runtime owns its commands and children; retained collaborators are
same-binary child runtimes. Typed events provide shared state to terminal,
browser and machine interfaces.

Linux and macOS are supported. The native runtime links libcurl and the platform
C++ libraries. The default build also embeds a browser interface and vendors
cpp-httplib alongside json.hpp; `-DUAGENT_WEB=OFF` retains a CLI-only build.
Optional native Web Push adds OpenSSL 3 libcrypto. See [the web guide](docs/WEB.md)
for architecture, security, dependencies and limits.

## Design

- One runtime per interactive conversation. CLI and web clients send commands
  and consume the same ordered events; neither owns a separate conversation.
- Explicit adapters for Chat Completions, OpenAI Responses and Anthropic
  Messages, driven by declared route capabilities.
- One process supervisor per runtime for command I/O, background work and
  cleanup, with shared approval and sandbox policy.
- Bounded operation defaults and optional aggregate budgets, reported through
  inspectable configuration and per-turn/session statistics.
- Atomic conversation snapshots, a bounded metadata journal and an optional
  sensitive debug trace with distinct purposes.

## Quick start

Run `uagent --web` for one local control center across all your directories.
Open the printed URL and pair the browser using the single-use code. Sessions
run independently and can be resumed from desktop or mobile; trusted HTTPS is
required for phone installation and background notifications. The embedded UI
needs no npm or Node runtime. [Setup and development](docs/WEB.md).

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
- Repository tools, document/image input, web search, skills, memory,
  Playwright automation, and dynamically discovered MCP tools. MCP stdio
  requires the stateless `2026-07-28` protocol; there is no legacy downgrade.
- One typed application event spine with terminal, stable JSONL, sensitive
  debug, bounded session-journal, and in-process subscriber consumers, plus a
  shared input channel used by terminal and web clients.
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
| evidence and state | `memory`, `uagent` |
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
| Ctrl+C while idle | A second press detaches; another key cancels the gesture |
| `/models`, `/model` | Search or change model route |
| `/effort`, `/variant` | Change reasoning effort or OpenRouter routing |
| `/attach` | Queue or clear an attachment |
| `/context`, `/trace`, `/cost`, `/ps`, `/agents`, `/tools` | Inspect active state |
| `/compact`, `/sessions`, `/reset` | Manage context and sessions |
| `/init`, `/review`, `/diff` | Write AGENTS.md, review changes, or show the git diff |
| `/memory` | Show saved memory action, time, source, and redacted preview |
| `/verbose` | Toggle full reasoning and expanded bounded tool output |
| `/yolo` | Toggle automatic approval |
| `/help`, `/quit` | Show help or detach from the runtime |

Approval prompts accept `y` once or `a` for the session. Shell approval reuse is
scoped to the exact command payload; approving one `git`, shell, or interpreter
invocation cannot authorize another. Any other answer denies the call and sends
the denial to the model as steering.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Operations and limits](docs/OPERATIONS.md)
- [Tools](docs/TOOLS.md)
- [Persistence](docs/PERSISTENCE.md)
- [Web interface](docs/WEB.md)
- [Memory, skills and scheduled tasks](docs/MANAGEMENT.md)
- [System prompts](docs/SYSTEM_PROMPTS.md)
- [Prompt caching](docs/CACHING.md)
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
