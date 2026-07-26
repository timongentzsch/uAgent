# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A small C++20 coding agent for OpenAI-compatible APIs. One core binary, direct
HTTP, and bounded tools.

## Quick start

Requirements: CMake, a C++20 compiler, libcurl, and optional libedit.
`run_python` needs [uv](https://docs.astral.sh/uv/) only for third-party
packages; the default Chrome DevTools integration needs Node.js/npm.

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
| `run`, `wait_background`, `terminal_output` | Supervised and persistent processes |
| `run_python` | Isolated uv-backed Python with optional packages |
| `show_image` | Native terminal images in supported interactive terminals |
| `attach` | Read an image or document (PDF, Word, Excel, …) into model context |
| `web_search` | OpenRouter search side request |
| `chrome-devtools_*`, `chrome_session` | Browser automation and session selection |
| `task` | Depth-bounded isolated subagent |
| `checkpoint` | Cache-aware context fold |

Independent read-only tools may run concurrently. Writes, shell, network,
delegation, and MCP calls require approval unless `--yolo` is active. Tool
results, requests, processes, logs, costs, and retained history are bounded.
Every tool exposes the same optional `timeout` (foreground seconds); defaults
come from its registry entry or `UAGENT_TOOL_TIMEOUT=30`, and `0` uses the turn
limit. Slow web searches return after 5 seconds, continue in the background,
and are injected into the next model step; their hard cap is
`UAGENT_WEB_SEARCH_TIMEOUT=25`. A call can batch four queries. Search output is
bounded by `UAGENT_WEB_SEARCH_MAX_TOKENS=1200`; set
`UAGENT_WEB_SEARCH_MODEL` to route search through a cheaper OpenRouter model
and `UAGENT_WEB_SEARCH_EFFORT` only when that model needs an explicit effort.
The default two calls per turn can each batch four queries; change
`UAGENT_WEB_SEARCH_CALLS` for unusually broad research. Every backgrounded tool
returns one job id; `wait_background` can join either a process or side request.

`run` uses bash by default, accepts another `shell`, uses process groups, and
backgrounds slow commands. Subagents may delegate again while they stay under
`UAGENT_SUBAGENT_DEPTH=2` (`0` disables delegation), and exit with the parent.
If the coordinator tries to finish while a
required background task is still running, it waits for all required results
and continues the turn. `detach=true` keeps long-lived servers and their
rotating logs across sessions; `terminal_output` lists or reads them.
Unsupported native tool calling falls back to a strict text protocol.
Interactive traces show complete `run` commands, Python source, and their
returned output; model-visible results remain bounded.

`run_python` uses `uv run --isolated --no-project`, so it cannot rewrite the
workspace's Python project or inherit an active virtual environment. Package
requirements must be passed in its `packages` argument and are cached by uv;
installing them through `pip` or a separate shell cannot affect a later call.
Plotting uses a non-interactive backend; save
the image and open it with `show_image`. Without uv, standard-library code still
runs on `python3`; only `packages` requires uv.

## Context

Before the first request, µAgent loads project instructions from repository
root to the working directory. Each level loads one file, preferring
`AGENTS.override.md`, then `AGENTS.md`, then `CLAUDE.md` as a compatibility
fallback; `~/.uagent` is the global level. Later files are more specific.
`UAGENT_PROJECT_DOC_BYTES` sets the shared 32 KiB content limit (`0` disables
loading).

`/attach` adds files to your next message and `--attach PATH` (repeatable) sends
them with the first one; the model reaches the same encoder itself with `attach`,
up to four files per turn. Images travel as `image_url`
data URLs and documents as `file_data`, bounded by `UAGENT_ATTACHMENT_MB=10`.
Encoded bytes are dropped from history once the turn that carried them ends.

The system prompt, project instructions, and ordered tool schemas stay stable
for provider caching.
Completed tool traces move to a bounded local archive.
The local date, time, timezone, and UTC offset refresh once per user turn and
remain fixed through that turn's model/tool rounds. They are appended with the
turn rather than written into the system prompt, so no turn rewrites the cached
prefix.

Models write math as Unicode text. TTY output also preserves raw
LaTeX delimiters while coloring inline/display math, including math and code
spans containing pipes in tables.

At 65% projected context, the model may create a durable checkpoint; at 85% the
request becomes urgent, and a model that ignores two urgent requests is compacted
for it. Default `apply` mode commits a valid checkpoint only
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
`UAGENT_CHROME_BROWSER_URL` to an explicit DevTools endpoint. Set
`UAGENT_CHROME_DEVTOOLS=0` to disable the built-in. A `chrome-devtools` entry in
`~/.mcp.json` or a trusted project config overrides it.
Image content returned by any MCP server is saved privately under
`~/.uagent/mcp`, rendered inline when the terminal supports it, and never copied
as base64 into model history.

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

Each response footer reports output `tok/s` and time to the first model event.

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

`/model` remembers the selected route across restarts in a private preference
file. An explicit `UAGENT_MODEL` still overrides it.

Escape interrupts the active response and opens steering; a second Escape
resumes. Sessions are saved under `~/.uagent/history`. Debug JSONL traces are
opt-in with `--debug[=PATH]` and may contain private source and reasoning.

## Development

Python is test-only and managed with [uv](https://docs.astral.sh/uv/):

```sh
uv sync --frozen
uv run --frozen ruff check tests
uv run --frozen ruff format --check tests
uvx --from clang-format==22.1.8 clang-format -i \
  include/*.h src/*.cc tests/*.cc benchmarks/*.cc

cmake -S . -B build -DUAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

First-party C++ uses the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) and
the checked-in exact Google `.clang-format` style. CI enforces Google
`clang-tidy` checks, identifier naming, control-flow rules, and
`cpplint`; the C++ build disables exceptions. The sole suppressed integer-type
warning is the `short` required by the POSIX spawn ABI. Vendored code under
`third_party/` is excluded from first-party style checks.

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
