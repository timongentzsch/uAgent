# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

µAgent started as a small, practical coding assistant and grew into a harness
for exploring model tool use, provider behavior, and custom agent workflows.

It is a lean, modular C++20 binary with direct HTTP, bounded tools, optional
MCP, and no language runtime or framework. It runs across supported Linux and
macOS architectures, while structured traces make every workflow easy to
inspect, compare, and improve.

## Quick start

Requires CMake, a C++20 compiler, and libcurl. Node.js enables Chrome DevTools
MCP, and
[uv](https://docs.astral.sh/uv/) enables Python scratch scripts and route
evaluation.

```sh
./install.sh
```

Create `~/.uagent/.config`:

```dotenv
OPENROUTER_API_KEY=replace-me
OPENROUTER_MODEL=deepseek/deepseek-v4-flash
```

For any OpenAI-compatible endpoint, set `UAGENT_BASE_URL`, `UAGENT_API_KEY`,
and `UAGENT_MODEL`. `UAGENT_PROVIDERS` defines named routes; set a provider's
`protocol` to `openrouter` only for an OpenRouter-compatible proxy. Environment
values override the private config file; project `.env` files are never loaded.
The bundled `$uagent-config` skill gives the agent the complete configuration
reference for its release; ask it to explain, inspect, or update these settings.

```sh
uagent
uagent -p "inspect this repository"
uagent -p "inspect this repository" --json
uagent -p "inspect this repository" --json-stream --budget 2
uagent -p "inspect this repository" --no-memory
uagent --debug=/tmp/uagent.jsonl
uagent --resume
uagent --yolo
```

## Observe and compare

The persistent two-line composer keeps native scrollback visible. Idle status
shows model, effort, endpoint, context, cache, cost, and active background
count; the single status row switches in place to show current harness
activity, elapsed time, live context, queued steering, background count, and
the Escape hint.
`/context` prints the current model-shaped context and registered tool schemas;
`/trace` prints the latest tool/search exchange; `/cost` breaks spend down by
route; `/ps` lists the active work behind `bg:N`. `--debug`
records reconstructable messages, responses, tools, timing, usage, and failures.
The same evidence makes regressions attributable and guides prompt, tool,
routing, and orchestration improvements instead of relying on anecdote.

`--json` emits one stable `uagent.headless.v1` envelope containing `answer`,
`error`, `trace`, `usage`, `routes`, and `exit_code`. `--json-stream` emits
versioned lifecycle JSONL. Usage distinguishes reported zero cost from
unavailable provider cost.

Response `tok/s` counts all provider-reported generated tokens, including
hidden reasoning. Plain-text streams use time after the first token; reasoning
responses use the full request because hidden reasoning is generated before the
first visible summary or answer event. `first` is client-observed time to that
first semantic stream event.

## Capabilities

| Area | Built-ins |
| --- | --- |
| repository | `read_file`, `list_dir`, `grep`, `write_file`, `edit_file` |
| execution | `run`, detached terminals, uv-backed `run_python` |
| evidence | attachments, terminal images, cited `web_search` |
| extension | MCP, Chrome DevTools, skills, project/global memory |
| orchestration | parallel tools, bounded `task` routes, activity wait/stop, automatic compaction |

Mutating, shell, network, delegation, and MCP calls require approval unless
`--yolo` is active. Child processes are credential-sanitized. Inputs, outputs,
processes, context, persistence, and reported spend are bounded.
Model labels include reasoning effort; delegated task labels also include their
provider. Working status reports live context and the number of active
background processes.
Commands, tasks, and services share stable activity IDs. `activity_output`
inspects bounded logs; `wait_ms` blocks for new output or exit, and `until`
waits for a fixed readiness marker. `task(background=false)` returns a required
child result in the current model step; background tasks return immediately and
deliver their result automatically on exit. `activity_wait` remains available
when no useful parent work can continue. `activity_stop` stops the complete
owned process group and cleans its detached record and logs. Interactive and
headless runs both continue when required background work finishes. Child-side
buffering may delay visible output. Enter queues guidance into the active turn
at its next safe boundary. Escape interrupts only the foreground operation;
delegated and detached work keeps running unless explicitly cancelled.

Memory uses progressive disclosure: startup injects bounded names, and the
model can list, search, or fetch bodies on demand. Writes happen only when the
user explicitly asks. Disable recall and writes with `--no-memory`.

At 85% projected context pressure, including pending input and tool schemas,
µAgent atomically replaces history with a bounded non-authoritative summary. The same
path powers `/compact`; a failed summary leaves history unchanged. Old bulky
tool outputs are compacted in batches after their full trace is archived.
Sessions live under `~/.uagent/history`; project configuration requires
interactive trust or `--trust-project-config`.

## Commands

| Command | Action |
| --- | --- |
| `/models [QUERY]`, `/model MODEL` | Search or change route |
| `/effort LEVEL\|default` | Set reasoning effort |
| `/variant MODE\|default` | Set OpenRouter routing (`nitro`, `floor`, or `exacto`) |
| `/attach PATH\|clear` | Queue an attachment or clear the queue |
| `/memory` | Show memory state and saved keys |
| `/context`, `/trace`, `/cost` | Inspect request, execution, or spend |
| `/ps` | Show active background work |
| `/compact` | Fold active context |
| `/sessions`, `/reset` | Resume or reset state |
| `/verbose`, `/online`, `/yolo` | Toggle runtime behavior |
| `/help`, `/quit` | Help or exit |

The input bar remains editable while work runs. Enter adds guidance to the
active turn's steering queue; it is applied after the current model response or
tool boundary, not started as a separate next turn. Escape interrupts the
foreground operation and applies queued guidance immediately without cancelling
background work. Multiline paste is inserted atomically, and switching terminal
focus preserves the current draft. Option/Alt word movement remains an editor
action rather than being mistaken for bare Escape. A single status row
stays pinned immediately above the input and changes in place between the idle
model/session summary and the current harness state.

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
See [architecture](docs/ARCHITECTURE.md), [operations](docs/OPERATIONS.md),
[persistence](docs/PERSISTENCE.md), [testing](docs/TESTING.md), and
[contributing](CONTRIBUTING.md).

µAgent is a local single-user POSIX CLI, not an OS sandbox.
