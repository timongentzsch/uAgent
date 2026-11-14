# µAgent

[![CI](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml/badge.svg)](https://github.com/timongentzsch/uAgent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A native coding agent for local and SSH projects, with terminal and web interfaces.

Keep conversations, settings and model credentials on your main machine. Work on
local files or execute on a remote host, and follow the same conversation from a
terminal, desktop browser or phone. Device pairing and SSH provide access without
a µAgent account or relay service; model providers may require their own accounts.

![Terminal and phone working on a stock reservation feature](docs/images/terminal.gif)

*Recorded terminal and web UI with a scripted model response; file edits, tests
and approvals run through µAgent.*

## Features

- **Remote execution, main-owned state.** Select an SSH host when opening a
  project. µAgent installs a small execution gateway; files, Git, commands and
  terminals run there. Provider requests and keys stay on the main.
- **One conversation across devices.** Continue in the terminal or paired web
  app, including approvals from a phone. No conversation export or restart.
- **Browser automation with manual takeover.** Watch Chrome and take control
  when needed. Password-manager fills are masked in the agent's browser tools;
  see the [security boundaries](docs/BROWSER.md#isolation).
- **Provider choice.** Use local or hosted models through OpenAI-compatible
  Chat Completions, OpenAI Responses or Anthropic Messages APIs. Setup can
  discover current free OpenRouter models that support tools.
- **Parallel work and review.** Delegate tasks and inspect agent reports,
  progress, spend and changes in the folder overview.
- **Execution controls and reusable context.** OS sandboxing, approval modes,
  undo, skills, memory, MCP and scheduled tasks.

## Comparison

Remote access, multiple providers and parallel agents are available in other
coding tools too. The distinction here is the combination of a native runtime,
paired interfaces and a built-in execution-only SSH gateway.

| Tool | Relevant difference |
| --- | --- |
| µAgent | The main runs the agent and provider calls; SSH hosts execute project operations without provider configuration. |
| [OpenCode](https://opencode.ai/docs/cli/web/) | Its documented remote setup runs the agent server on the remote host and connects terminal/web clients to it; it also supports pairing and SSH forwarding. |
| [Claude Code Remote Control](https://code.claude.com/docs/en/remote-control) | Continues a local session through Claude's web/mobile interfaces; requires a Claude account with an eligible subscription. |

Comparison checked 2026-11-10 against the linked documentation. It describes
these workflows, not every plugin or deployment option.

## Resource use

A C++20 executable with its web interface embedded. No JavaScript runtime is
required to run µAgent. Chrome starts when browser work needs it.

![Idle memory and CPU per headless turn compared with Claude Code, Codex CLI and omp](docs/images/comparison.svg)

The same small task, model endpoint and container limits. Five-run medians,
measured 2026-11-10: **26.4 MiB idle memory** and **17 ms CPU per headless turn**
for µAgent. This measures agent overhead, not model quality or response speed.
The Linux ARM64 complete installation is **22.6 MB**, including remote gateways
and the sheet engine.
[Versions, raw results and method](benchmarks/README.md#footprint-comparison).

## Get started

```sh
curl -fsSL https://raw.githubusercontent.com/timongentzsch/uAgent/master/install.sh | sh
```

Choose a local or container install, then connect a provider. The optional free
OpenRouter setup requires an OpenRouter account: sign in from the home screen or
with `uagent setup openrouter`, with no key to copy. It lists eligible models
from the current catalogue. Open a project:

```sh
cd my-project
uagent
```

Use `uagent --web` to pair your browser or phone. Add an SSH host under
**Remote hosts**, then select it when starting a conversation or opening a folder.
The main must be reachable from your device, for example over your LAN or VPN.

Linux and macOS, x86_64 and ARM64.
[Setup and platform requirements](docs/GUIDE.md#install) ·
[Free-model limits](docs/GUIDE.md#free-model-setup) ·
[Remote hosts and current limits](docs/REMOTE_HOSTS.md)

## Remote projects

![Devices connect to the main; the main calls providers and sends execution to SSH hosts](docs/images/remote-workflow.svg)

Multiple main instances can reuse the same remote executable with separate
execution processes. The gateway stores no application configuration or provider
keys. Sharing an SSH account still shares its ordinary filesystem permissions.
[Connection, recovery and supported tools](docs/REMOTE_HOSTS.md).

## Documentation

| Task | Reference |
| --- | --- |
| Start a conversation, choose a model, use terminal commands | [Guide](docs/GUIDE.md) |
| Pair devices and use the web interface | [Web interface](docs/WEB.md) |
| Connect SSH hosts and recover interrupted work | [Remote hosts](docs/REMOTE_HOSTS.md) |
| Use browser automation and manual takeover | [Browser](docs/BROWSER.md) |
| Manage context and recurring tasks | [Memory, skills and schedules](docs/MANAGEMENT.md) |
| Configure limits, budgets and recovery | [Operations](docs/OPERATIONS.md) |
| Understand ownership and trust boundaries | [Architecture](docs/ARCHITECTURE.md) · [Security](SECURITY.md) |

## Development

C++20 and a Preact web interface, with native unit/integration tests and
Chromium/WebKit coverage. Tests use mock providers and need no API key.

[Contributing](CONTRIBUTING.md) · [Testing](docs/TESTING.md) ·
[Benchmarks and raw measurements](benchmarks/README.md) · [Changelog](CHANGELOG.md)
