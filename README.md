# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

µAgent is a local coding agent in one native binary, `uagent`. Run it in a
terminal, from a script (`-p`, with JSON output) or in the browser
(`--web`); all three drive the same conversations. It talks to any Chat
Completions, OpenAI Responses or Anthropic Messages endpoint.

## Install

Linux and macOS. From a checkout:

```sh
./install.sh
```

This builds a Release binary in `build/release` (reusing a build already
there) and installs `uagent` and the bundled skills under `~/.local`; put
`~/.local/bin` on your `PATH`. `UAGENT_PREFIX` and `UAGENT_BUILD_DIR` change
the two locations.

Each tagged release also ships built archives
(`uagent-<version>-linux-x86_64`, `-linux-arm64`, `-macos-arm64`, each
`.tar.gz`). Unpack one and run `bin/uagent`; keep `bin` and `share` together,
since the skills are found relative to the binary.

### Requirements

- CMake 3.21+, a C++20 compiler and libcurl.
- Node.js, to build the browser interface from `web/`. Without it the build
  is terminal-only. Each release also ships the built interface as
  `uagent-web-dist-<version>.tar.gz`; unpack it into `web/dist` before
  building.
- Optional at run time: [uv](https://docs.astral.sh/uv/) for Python `scratch`
  scripts with dependencies; `npm install -g @playwright/cli@latest` for the
  `browser-use` skill.

| CMake option | Effect |
| --- | --- |
| `-DUAGENT_WEB=OFF` | Terminal-only binary, no browser interface |
| `-DUAGENT_BROWSER=OFF` | Leave out the browser appliance |
| `-DUAGENT_WEB_PUSH=ON` | Web Push notifications; needs OpenSSL 3 libcrypto. Release archives and the Docker image include it |

## Configure

Give it a key and start it in a project folder:

```sh
export OPENROUTER_API_KEY=replace-me
cd my-project
uagent
```

With only an OpenRouter key it uses a default model. Choose your own inside
the session; either line also saves it for new conversations:

```text
/model deepseek/deepseek-v4-flash --default
/config user UAGENT_MODEL=deepseek/deepseek-v4-flash
```

For any other OpenAI-compatible endpoint set `UAGENT_BASE_URL`,
`UAGENT_API_KEY` and `UAGENT_MODEL` instead:

```sh
export UAGENT_BASE_URL=http://localhost:8080/v1
export UAGENT_MODEL=my-model
```

`UAGENT_PROVIDERS` defines named providers, including ones that speak
OpenAI Responses or Anthropic Messages (`wire_api`).

Settings are saved by µAgent, not in files you edit:

| To | Use |
| --- | --- |
| see what is set and where from | `/config` |
| save for all conversations | `/config user KEY=VALUE` |
| save for this project folder | `/config project KEY=VALUE` |
| set this conversation's model or approval mode | `/config conversation KEY=VALUE` |
| remove a saved value | `/config user unset KEY` |
| move settings between hosts | `uagent config export`, `uagent config import FILE` |

The web's Settings edits the same values. Command-line flags override
`UAGENT_*` environment variables, which override what is saved for the
project, which overrides what is saved for all conversations. Nothing is read
from the project: `.env` files are never loaded, and a `~/.uagent/.config` or
`.uagent/.config` from an earlier version is taken over once and kept as
`.config.imported`.

Every setting and its default is in the
[configuration reference](skills/uagent-config/references/configuration.md);
inside a session, the bundled `$uagent-config` skill answers from it.

## Usage

```sh
uagent                                   # interactive session in this folder
uagent -c                                # continue the latest session
uagent --resume                          # pick a saved session
uagent --model MODEL --attach shot.png   # a model and a file for this run
uagent --yolo                            # approve changes without asking
uagent --plain                           # screen-reader output
```

Headless, for scripts. `-p` runs one turn and prints the final answer:

```sh
uagent -p "inspect this repository"
uagent -p "inspect this repository" --json          # one JSON envelope
uagent -p "inspect this repository" --json-stream   # JSONL events
uagent -p "fix the tests" --yolo --budget 2 --token-budget 20000
```

The `--json` envelope has `schema` (`uagent.headless.v1`), `answer`, `error`,
`stop`, `usage`, `routes`, `trace` and `exit_code`. `--budget` caps spend in
USD and `--token-budget` generated tokens. A failed run exits nonzero.

In the browser:

```sh
uagent --web                             # prints a URL and a pairing link
```

One host serves every folder's sessions on desktop and mobile. The pairing
code is single-use and valid for five minutes. See
[the web guide](docs/WEB.md) for remote access, the Docker browser appliance
and how the agent uses logins saved in that browser.

Several sessions in one folder: `uagent coord` (or `/coord`) opens the
folder's coordinator, which delegates work to threads and decides the
approvals they cannot settle. `/board` lists the folder's sessions and
`/open ID` switches to one.

`uagent --help` lists every flag; `--debug=PATH` writes a trace for bug
reports, which contains sensitive data.

## Highlights

- One runtime per conversation; terminal and browser clients send commands to
  it and render the same ordered events.
- Streamed answers and reasoning, queued steering while the agent works, and
  interruption at any point.
- Supervised processes with optional PTYs, writable input, background
  handoff and bounded logs, confined by an OS sandbox that restricts writes.
- File, search, shell, memory, web, skill, subagent and MCP tools, filtered
  by policy and route capabilities; see [Tools](docs/TOOLS.md).
- Approval modes (ask, auto, YOLO). YOLO stops the questions, not the
  sandbox, and changes to µAgent's own configuration and unsandboxed commands
  always need a person.
- `/changes` and `/undo` list and put back the files the last turn's edits
  changed.
- Bounded time, output, processes, context and persistence, plus optional
  turn budgets for spend, tokens and calls.

## Interactive controls

| Input | Action |
| --- | --- |
| Enter while working | Queue the draft as steering for the running turn |
| Shift+Enter, Alt+Enter | Insert a newline in the draft |
| Tab | Complete a `/command` or an `@path` segment |
| Up, Down in the `/` menu | Highlight a command; Tab or Enter takes it |
| Ctrl+X Ctrl+E | Edit the draft in `$VISUAL`/`$EDITOR` |
| Up, Down, Ctrl+P, Ctrl+N | Previous and next draft from history |
| Ctrl+A, Ctrl+E, Ctrl+F | Move to the start, to the end, one character right |
| Ctrl+W, Ctrl+K, Ctrl+U | Delete the word before the cursor, to the end, to the start |
| Esc | Clear the draft and interrupt the running turn |
| Ctrl+B | Move the foreground command to background supervision |
| Ctrl+C | Interrupt while working; press twice while idle to detach |
| Ctrl+D on an empty draft | Detach |

Approval prompts accept `y` (once), `s` (this session), `a` (always in this
repository) or `n`; any other answer denies the call and is sent to the model
as guidance. Changes that need a person accept only `y` or `n`. Remembered
shell approvals match the exact command, not the executable.

Common slash commands:

| Command | Action |
| --- | --- |
| `/model`, `/models`, `/effort`, `/variant` | Choose this conversation's route, model, reasoning effort or OpenRouter routing; `/model X --default` also saves it for new conversations |
| `/attach PATH`, `/diff`, `/review`, `/init` | Attach a file, show the git diff, review changes, write `AGENTS.md` |
| `/changes`, `/undo [FILE]` | List the files the last turn changed; put them back as they were |
| `/status`, `/context`, `/cost`, `/http` | Inspect configuration, the model request, spend and captured traffic |
| `/ps`, `/agents`, `/tools`, `/mcp`, `/permissions`, `/yolo` | Manage background work, delegated agents, tools, MCP servers and approval mode |
| `/sessions`, `/reset`, `/rename`, `/fork`, `/rewind`, `/compact`, `/share` | Resume a saved session, start a new one, rename, branch, edit an earlier message, summarize, export |
| `/coord`, `/board`, `/open ID` | Open the folder's coordinator, list its sessions, switch to one |
| `/memory`, `/skills`, `/schedule`, `/instructions`, `/config`, `/restart` | Manage memory, skills, scheduled tasks, instructions and settings; restart to apply one |
| `/btw QUESTION` | Ask a side question about the conversation; the answer is not added to it |
| `/verbosity LEVEL`, `/clear`, `/help`, `/quit` | Show minimal, default or full detail everywhere; clear the screen, list all commands, detach |

`/help` lists every command with its arguments.

## Documentation

- [Web interface](docs/WEB.md)
- [Tools](docs/TOOLS.md)
- [Memory, skills and scheduled tasks](docs/MANAGEMENT.md)
- [Bundled skills](skills/README.md)
- [Operations and limits](docs/OPERATIONS.md)
- [Persistence](docs/PERSISTENCE.md)
- [Accessibility](docs/ACCESSIBILITY.md)
- [Security](SECURITY.md)
- Reference, generated from the binary:
  [flags](skills/uagent-config/references/cli.md),
  [slash commands](skills/uagent-config/references/slash-commands.md),
  [settings](skills/uagent-config/references/configuration.md)
- Internals: [Architecture](docs/ARCHITECTURE.md),
  [System prompts](docs/SYSTEM_PROMPTS.md),
  [Prompt caching](docs/CACHING.md)
- [Changelog](CHANGELOG.md)

## Development

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
```

See [Contributing](CONTRIBUTING.md) for the checks CI runs and
[Testing](docs/TESTING.md) for running one test.
