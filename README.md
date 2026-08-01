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

Requires CMake, a C++20 compiler, and libcurl. libedit enables richer input,
Node.js enables Chrome DevTools MCP, and [uv](https://docs.astral.sh/uv/) enables
Python scratch scripts and route evaluation.

```sh
./install.sh
```

Create `~/.uagent/.config`:

```dotenv
OPENROUTER_API_KEY=replace-me
OPENROUTER_MODEL=deepseek/deepseek-v4-flash
```

For any OpenAI-compatible endpoint, set `UAGENT_BASE_URL`, `UAGENT_API_KEY`,
and `UAGENT_MODEL`. `UAGENT_PROVIDERS` defines named routes. Environment values
override the private config file; project `.env` files are never loaded.

```sh
uagent
uagent -p "inspect this repository"
uagent -p "inspect this repository" --json
uagent -p "inspect this repository" --json-stream --budget 2
uagent --debug=/tmp/uagent.jsonl
uagent --resume
uagent --yolo
```

## Observe and compare

Interactive status shows the route, context, tokens, cache, cost, and timing.
`/context` prints the exact model request and tool schemas; `/trace` prints the
latest tool/search exchange; `/cost` breaks spend down by route. `--debug`
records reconstructable messages, responses, tools, timing, usage, and failures.
The same evidence makes regressions attributable and guides prompt, tool,
routing, and orchestration improvements instead of relying on anecdote.

`--json` emits one stable `uagent.headless.v1` envelope containing `answer`,
`error`, `trace`, `usage`, `routes`, and `exit_code`. `--json-stream` emits
versioned lifecycle JSONL. Usage distinguishes reported zero cost from
unavailable provider cost.

`uagent eval` gives every model a fresh workspace, home, session, fixture,
prompt contract, and cost ceiling:

```sh
uagent eval \
  --model deepseek/deepseek-v4-flash \
  --model anthropic/claude-sonnet-4.5 \
  --scenario checkpoint --repetitions 3 \
  --report /tmp/uagent-route-ab.json
```

Reports include violations, tokens, reported cost, wall time, and peak RSS.
Fresh multi-sample results update expiring route profiles, allowing runtime to
disable unsupported checkpoint, parallel-call, or image behavior before work.
See [testing](docs/TESTING.md) for scenarios and extension.

## Capabilities

| Area | Built-ins |
| --- | --- |
| repository | `read_file`, `list_dir`, `grep`, `write_file`, `edit_file` |
| execution | `run`, detached terminals, uv-backed `run_python` |
| evidence | attachments, terminal images, cited `web_search` |
| extension | MCP, Chrome DevTools, skills, project/global memory |
| orchestration | parallel tools, bounded `task`, checkpoint and handoff |

Mutating, shell, network, delegation, and MCP calls require approval unless
`--yolo` is active. Child processes are credential-sanitized. Inputs, outputs,
processes, context, persistence, and reported spend are bounded.

At context pressure, µAgent can fold history into a bounded non-authoritative
checkpoint. `/handoff PROVIDER/MODEL` uses that clean cache boundary to change
routes. Sessions live under `~/.uagent/history`; project configuration requires
interactive trust or `--trust-project-config`.

## Commands

| Command | Action |
| --- | --- |
| `/models [QUERY]`, `/model MODEL` | Search or change route |
| `/effort LEVEL\|default` | Set reasoning effort |
| `/attach PATH`, `/detach` | Manage attachments |
| `/context`, `/trace`, `/cost` | Inspect request, execution, and spend |
| `/compact`, `/handoff ROUTE` | Fold context or change route |
| `/sessions`, `/reset` | Resume or reset state |
| `/verbose`, `/online`, `/yolo` | Toggle runtime behavior |
| `/help`, `/quit` | Help or exit |

Escape interrupts an active response and opens steering; a second Escape
resumes.

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
