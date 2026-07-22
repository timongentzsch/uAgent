# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A small C++17 coding agent for OpenAI-compatible APIs. One core binary, direct
HTTP, and bounded tools.

## Quick start

Requirements: CMake, a C++17 compiler, libcurl, and optional libedit. The
`run_python` tool additionally needs [uv](https://docs.astral.sh/uv/); the
default Chrome DevTools integration needs Node.js/npm.

```sh
./install.sh
```

This builds a release binary and installs it to `~/.local/bin`. Set
`UAGENT_PREFIX` or `UAGENT_BUILD_DIR` to override either location, and ensure
`~/.local/bin` is on `PATH`.

Create `~/.uagent/.config`:

```dotenv
OPENROUTER_API_KEY=replace-me
OPENROUTER_MODEL=deepseek/deepseek-v4-flash
```

The file is forced to `0600`. Process environment variables override it; project
`.env` files are never loaded. `OPENROUTER_EFFORT` is optional; leaving it unset
uses the model's catalog default. `UAGENT_PROVIDERS` remains available for named
models on additional OpenAI-compatible endpoints, with optional `effort` and
`context` per model. The legacy `UAGENT_BASE_URL`, `UAGENT_API_KEY`, and raw
`UAGENT_MODEL` form still works.

```sh
uagent
uagent -p "inspect this repository"
uagent -c
uagent --resume
uagent --yolo
```

## Tools

| Tool | Purpose |
| --- | --- |
| `read_file`, `list_dir`, `grep` | Bounded repository inspection |
| `write_file`, `edit_file` | Atomic file changes |
| `run_bash`, `wait_background` | Supervised processes |
| `run_python` | Isolated uv-backed Python with optional packages |
| `view_image` | iTerm2, WezTerm, or Kitty image output |
| `web_search` | OpenRouter search side request |
| `chrome-devtools_*`, `chrome_session` | Browser automation and session selection |
| `task` | One-level isolated subagent |
| `checkpoint` | Cache-aware context fold |

Independent read-only tools may run concurrently. Writes, shell, network,
delegation, and MCP calls require approval unless `--yolo` is active. Tool
results, requests, processes, logs, costs, and retained history are bounded.
Every tool exposes the same optional `timeout` (foreground seconds); defaults
come from its registry entry or `UAGENT_TOOL_TIMEOUT=30`, and `0` uses the turn
limit. Slow web searches return after 5 seconds, continue in the background,
and are injected into the next model step; their hard cap is
`UAGENT_WEB_SEARCH_TIMEOUT=25`.

`run_bash` uses process groups and backgrounds slow commands. Subagents cannot
recurse and exit with the parent. If the coordinator tries to finish while a
required background task is still running, it waits for all required results
and continues the turn. Long-lived shell/Python jobs remain explicitly detached.
Unsupported native tool calling falls back to a strict text protocol.

`run_python` uses `uv run --isolated --no-project`, so it cannot rewrite the
workspace's Python project or inherit an active virtual environment. Package
requirements must be passed in its `packages` argument and are cached by uv;
installing them through `pip` or a separate shell cannot affect a later call.
Plotting uses a non-interactive backend; save
the image and open it with `view_image`. If uv is missing, the tool returns a
short installation link.

## Context

The system prompt and ordered tool schemas stay stable for provider caching.
Completed tool traces move to a bounded local archive.
The local date, time, timezone, and UTC offset refresh once per user turn and
remain fixed through that turn's model/tool rounds.

At 65% projected context, the model may create a durable checkpoint; at 85% the
request becomes urgent. Default `apply` mode commits a valid checkpoint only
before the next user message. Invalid or declined checkpoints leave history
unchanged. Set `UAGENT_CHECKPOINT_MODE=shadow` to evaluate a new model route.

See [checkpoint design](docs/CHECKPOINTS.md).

## MCP

µAgent implements bounded stdio MCP tools: initialization, paged tool listing,
calls, cancellation, list changes, structured results, and cleanup. It does not
implement HTTP transport, OAuth, resources, prompts, sampling, roots,
elicitation, or MCP tasks.

User configuration lives in `~/.mcp.json`. Project configuration requires
interactive trust or `--trust-project-config`.

[Chrome DevTools MCP](https://github.com/ChromeDevTools/chrome-devtools-mcp) is
enabled by default using npm's latest release. Its browser starts lazily in a
fresh isolated profile. The agent can call `chrome_session` with `mode: user`
when it needs an existing login; Chrome 144+ must be running, remote debugging
must be enabled at `chrome://inspect/#remote-debugging`, and Chrome asks you to
approve the connection only when the agent first interacts with it. Set
`UAGENT_CHROME_MODE=user` to start in that mode or
`UAGENT_CHROME_DEVTOOLS=0` to disable the built-in. A `chrome-devtools` entry in
`~/.mcp.json` or a trusted project config overrides it.

```json
{
  "mcpServers": {
    "example": {
      "command": "npx",
      "args": ["-y", "@example/mcp-server"],
      "env": {"TOKEN": "${EXAMPLE_TOKEN}"}
    }
  }
}
```

## Interactive use

Each response footer reports output `tok/s` and time to first token (`ttt`).

| Command | Action |
| --- | --- |
| `/attach PATH`, `/detach` | Manage next-turn attachments |
| `/sessions`, `/reset` | Resume or reset a session |
| `/models [FILTER\|all]`, `/model MODEL` | Query or switch models |
| `/effort LEVEL\|default` | Override or restore provider reasoning effort |
| `/compact` | Summarize active history |
| `/online` | Toggle OpenRouter online mode |
| `/yolo` | Toggle automatic approval |
| `/quit` | Exit |

Escape interrupts the active response and opens steering; a second Escape
resumes. Sessions are saved under `~/.uagent/history`. Debug JSONL traces are
opt-in with `--debug[=PATH]` and may contain private source and reasoning.

## Development

Python is test-only and managed with [uv](https://docs.astral.sh/uv/):

```sh
uv sync --frozen
uv run --frozen ruff check tests
uv run --frozen ruff format --check tests

cmake -S . -B build -DUAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The optional live harness spends API credit and uses a checksum-pinned Astropy
SWE-bench fixture:

```sh
python3 tests/agent_workflow_live.py --run
python3 tests/agent_workflow_live.py --run --scenario checkpoint500k
```

Architecture, limits, and release checks are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/OPERATIONS.md](docs/OPERATIONS.md).

µAgent is a local single-user POSIX CLI, not an OS sandbox.
