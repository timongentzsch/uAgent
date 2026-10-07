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
since the skills are found relative to the binary. `SHA256SUMS`, signed
with Sigstore (`SHA256SUMS.sigstore.json`), lists every archive's checksum.

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
the session; `--default` also saves it for new conversations:

```text
/model deepseek/deepseek-v4-flash --default
```

For any other OpenAI-compatible endpoint set `UAGENT_BASE_URL`,
`UAGENT_API_KEY` and `UAGENT_MODEL` instead:

```sh
export UAGENT_BASE_URL=http://localhost:8080/v1
export UAGENT_MODEL=my-model
```

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
project, which overrides what is saved for all conversations. Saved settings
live outside the project, and `.env` files are never loaded. A
`~/.uagent/.config` or a trusted project's `.uagent/.config` from an earlier
version is imported once and kept as `.config.imported`.

Every setting and its default, named providers (`UAGENT_PROVIDERS`)
included, is in the
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
`/open ID` switches to one. Ask it for other voices and it adds members to
the conversation, each with a persona and, if you like, its own model.
Everyone there hears every message and decides whether to answer, to wait
for someone who is typing, or to say nothing; `@name` asks one of them.

`uagent --help` lists every flag; `--debug=PATH` writes a trace for bug
reports, which contains sensitive data.

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

Slash commands for a first session:

| Command | Action |
| --- | --- |
| `/model NAME` | Choose this conversation's model; `--default` also saves it |
| `/attach PATH` | Attach a file |
| `/changes`, `/undo [FILE]` | List the files the last turn changed; put them back as they were |
| `/permissions ask\|auto\|yolo` | Choose how calls are approved |
| `/sessions`, `/reset` | Resume a saved session; start a new one |
| `/config` | Show and change settings |
| `/help`, `/quit` | List every command with its arguments; detach |

The full list is in the
[slash-command reference](skills/uagent-config/references/slash-commands.md).

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

See [Contributing](CONTRIBUTING.md) for building, the checks CI runs and
[Testing](docs/TESTING.md) for running one test.
