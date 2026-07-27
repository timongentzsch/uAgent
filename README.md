# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A small C++20 coding agent for OpenAI-compatible APIs: one binary, direct HTTP,
bounded tools, and optional MCP integration.

## Quick start

Requires CMake, a C++20 compiler, libcurl, and optionally libedit. Node.js/npm
enables the default Chrome DevTools integration; [uv](https://docs.astral.sh/uv/)
enables third-party packages in `run_python`.

```sh
./install.sh
```

This installs `uagent` to `~/.local/bin`. Set `UAGENT_PREFIX` or
`UAGENT_BUILD_DIR` to override the install or build directory.

Create `~/.uagent/.config`:

```dotenv
OPENROUTER_API_KEY=replace-me
OPENROUTER_MODEL=deepseek/deepseek-v4-flash
```

The config file is kept at `0600`; environment variables override it, and
project `.env` files are never loaded. Named OpenAI-compatible routes can be
defined with `UAGENT_PROVIDERS`. Legacy `UAGENT_BASE_URL`, `UAGENT_API_KEY`,
and `UAGENT_MODEL` configuration remains supported.

A workspace opts into its own settings by creating a `.uagent` directory. Its
`.config` then wins key by key, with `~/.uagent/.config` supplying the rest, and
`run_python` keeps its uv environments in `.uagent/uv` instead of the user-level
one. Because that file can redirect every request, it needs the same trust as
`.mcp.json` — an interactive prompt or `--trust-project-config`. Sessions, logs,
and history stay global. µAgent never creates `.uagent` in a workspace on its
own; you do, or the agent does when it saves a project memory.

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
| `read_file`, `list_dir`, `grep` | Inspect the repository |
| `write_file`, `edit_file` | Make atomic file changes |
| `run`, `wait_background`, `terminal_output` | Manage supervised processes |
| `run_python` | Run isolated Python with optional uv packages |
| `show_image`, `attach` | View native terminal images or add files to context |
| `web_search` | Search through an OpenRouter side request |
| `chrome-devtools_*`, `chrome_session` | Automate Chrome; returned images attach to the model |
| `task` | Delegate to a depth-bounded subagent |
| `memory` | Keep a lesson for later sessions, per project or global |
| `skill` | Open a stored procedure from `~/.uagent/skills` or the project |
| `checkpoint` | Fold context into a durable checkpoint |

Independent read-only tools can run concurrently. Mutating, shell, network,
delegation, and MCP calls require approval unless `--yolo` is active. Requests,
results, processes, logs, costs, and retained history are bounded.

Slow commands and web searches continue in the background. `run_python` uses
`uv run --isolated --no-project`, so packages must be listed in the tool call.
`show_image` is available only when the terminal supports a native image
protocol.

## Context and sessions

Before the first request, µAgent loads one instruction file per directory from
the repository root to the working directory, preferring `AGENTS.override.md`,
then `AGENTS.md`, then `CLAUDE.md`. `~/.uagent` supplies global instructions.
Everything loaded is listed at startup.

The `memory` tool appends to the same context: one markdown file per lesson
under `<base>/memory`, global in `~/.uagent` or scoped to a workspace in its
`.uagent`. Instruction files are what you tell the agent; memories are what it
concluded, so they load last and are trimmed first. Delete a file to retract it.

Use `/attach PATH` or repeat `--attach PATH` to add images and documents.
Attachments are size-bounded and their encoded data is removed after the turn.
The prompt and ordered tool schemas remain stable for provider caching.

At 65% projected context the model may checkpoint; at 85% the request becomes
urgent. See [checkpoint design](docs/CHECKPOINTS.md).

Sessions are stored under `~/.uagent/history`. Debug JSONL traces are opt-in
with `--debug[=PATH]` and may contain private source and reasoning.

## Skills

A skill is a directory under `<base>/skills` holding a `SKILL.md`: YAML front
matter with a `description` and usually a `name`, then the procedure. The
directory name is authoritative. Only the front matter is read at startup — it
becomes one line in the `skill` tool's schema — and the body reaches the model
when it opens that skill. Owning many skills therefore costs a line of schema
each, not a document each.

`SKILL.md` is an open format that around thirty agents read, so a skill
installed for any of them already works here. Directories are searched in
increasing precedence, deduplicated by name:

```
~/.agents/skills  ~/.claude/skills  ~/.codex/skills  ~/.uagent/skills
./.agents/skills  ./.claude/skills  ./.codex/skills  ./.uagent/skills
```

A workspace skill therefore overrides a user one, and µAgent's own overrides a
vendor copy of the same name. `UAGENT_SKILL_PATH` replaces the list outright
with a colon-separated one. Startup reports how many were found and what their
descriptions cost per request, since that part of a skill is always sent.
If the configured count limit is reached, higher-precedence entries displace
lower-precedence ones.

`SKILL.md` may reference sibling files; the tool result names the skill's
directory so relative paths resolve. Skills add no privilege of their own — a
skill that says to run a script does so through `run`, which still asks. Like
`AGENTS.md`, a project skill is instructions from the repository, so read one
before trusting a checkout.

`install.sh` installs the bundled skills in `skills/` into `~/.uagent/skills`,
never overwriting one already there.

## MCP and Chrome

User MCP configuration lives in `~/.mcp.json`. Project configuration requires
interactive trust or `--trust-project-config`.

[Chrome DevTools MCP](https://github.com/ChromeDevTools/chrome-devtools-mcp) is
enabled by default and starts lazily in an isolated profile. For an existing
login, use `chrome_session` with `mode: user` and enable Chrome remote debugging
at `chrome://inspect/#remote-debugging`. `UAGENT_CHROME_MODE=user` makes that
the default; `UAGENT_CHROME_DEVTOOLS=0` disables the built-in integration.

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

## Interactive commands

| Command | Action |
| --- | --- |
| `/attach PATH`, `/detach` | Manage next-turn attachments |
| `/sessions`, `/reset` | Resume or reset a session |
| `/models [FILTER\|all]`, `/model MODEL` | Query or switch models |
| `/effort LEVEL\|default` | Set provider reasoning effort |
| `/compact` | Summarize active history |
| `/online` | Toggle OpenRouter online mode |
| `/yolo` | Toggle automatic approval |
| `/quit` | Exit |

`/model` persists the selected route unless `UAGENT_MODEL` is set. Escape
interrupts an active response and opens steering; a second Escape resumes.

## Development

```sh
uv sync --frozen
uv run --frozen ruff check tests
uv run --frozen ruff format --check tests

cmake -S . -B build -DUAGENT_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

First-party C++ follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
CI enforces the checked-in formatter, `clang-tidy`, identifier and control-flow
rules, `cpplint`, warnings-as-errors builds, and tests on Linux and macOS.

See [architecture](docs/ARCHITECTURE.md) and
[operations](docs/OPERATIONS.md) for design, limits, configuration, and release
checks.

µAgent is a local single-user POSIX CLI, not an OS sandbox.
