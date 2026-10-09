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

- CMake 3.21+, a C++20 compiler and libcurl 7.66+.
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

## Quick start

1. **Give it a key.** With an [OpenRouter](https://openrouter.ai) key and
   nothing else it picks a default model:

   ```sh
   export OPENROUTER_API_KEY=replace-me
   ```

   For a local or other OpenAI-compatible endpoint set `UAGENT_BASE_URL` and
   `UAGENT_MODEL` instead (and `UAGENT_API_KEY` if it wants one). Responses
   and Anthropic Messages endpoints are set up as
   [named providers](docs/GUIDE.md#models-and-providers).

2. **Start it in a project folder** and say what you want:

   ```sh
   cd my-project
   uagent
   ```

   Enter sends, Esc stops a turn, `/` lists the commands.

3. **When it asks before acting**, answer `y` (once), `s` (for this
   conversation), `a` (always in this repository) or `n`. It asks before it
   changes a file, runs a command or uses the network. Commands run in a
   sandbox that can write only inside the folder. `/undo` puts back the files
   the last turn changed.

4. **Pick a model.** `/models` searches what your key offers;
   `/model NAME --default` also keeps the choice for new conversations.

5. **Leave and come back.** `/quit` leaves the conversation running;
   `uagent -c` continues the latest and `uagent --resume` lets you pick.

6. **In the browser or on a phone.** `uagent --web` prints a link that pairs
   the browser; one host serves every folder's conversations. See the
   [web guide](docs/WEB.md).

7. **Several at once.** `uagent coord` opens the folder's coordinator. Ask it
   for work and it starts threads that do it; ask it for another voice and it
   adds a member to the chat. Start a message with a name ("Ada, …") to ask
   that one alone.

8. **From a script.** `uagent -p "inspect this repository" --json` runs one
   turn and prints one JSON envelope.

The [guide](docs/GUIDE.md) tells the rest in order: keys, models and
providers, settings, approvals, coming back to a conversation, the
coordinator and its chat, and scripting. `uagent --help` lists every flag.

## Documentation

- [Guide](docs/GUIDE.md): the whole story, in order
- [Web interface](docs/WEB.md) and the [browser the agent drives](docs/BROWSER.md)
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
  [Instructions](docs/INSTRUCTIONS.md),
  [Prompt caching](docs/CACHING.md)
- [Changelog](CHANGELOG.md)

## Development

See [Contributing](CONTRIBUTING.md) for building, the checks CI runs and
[Testing](docs/TESTING.md) for running one test.
