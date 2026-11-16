# Documentation

For someone new to the code, read in this order: the
[overview](architecture/overview.md), the [turn loop](architecture/turn-loop.md),
[tools](architecture/tools.md), then the subsystem you came for.

## Architecture

How each subsystem works and why. Every page has the same seven sections:
responsibility, processes and threads, data and formats, protocol, invariants
and limits, failure modes, source and tests.

| Page | Covers | Source |
| --- | --- | --- |
| [Overview](architecture/overview.md) | processes, channels, build layers | `CMakeLists.txt`, `src/app/` |
| [Turn loop](architecture/turn-loop.md) | a turn, steps, compaction, sub-agents | `src/agent/` |
| [Providers](architecture/providers.md) | wire adapters, streams, the cached prefix | `src/api/`, `src/providers/` |
| [Tools](architecture/tools.md) | executor, approvals, activities, MCP | `src/tools/`, `src/mcp/` |
| [Sandbox](architecture/sandbox.md) | Seatbelt and Landlock policies, levels | `src/core/` |
| [Sessions](architecture/sessions.md) | journal, recovery, undo, retention | `src/agent/` |
| [Web host](architecture/web-host.md) | pairing, events, sockets, preview origin | `src/web/`, `src/app/` |
| [Web client](architecture/web-client.md) | state ownership, reconnection, layout | `web/src/` |
| [Execution gateway](architecture/execution-gateway.md) | remote execution over SSH | `src/app/`, `src/web/` |
| [Browser](architecture/browser.md) | the Chrome service, leases, the viewer | `src/browser/` |
| [Artifacts](architecture/artifacts.md) | frames, the sheet engine, saving | `web/sheet/`, `web/src/` |
| [Memory and schedules](architecture/memory-and-schedules.md) | memory, skills, the scheduler | `src/agent/`, `src/app/` |

## Interfaces

Contracts. The first four are generated from the binary's registries by
`tests/reference_docs.py`, and a test fails when they differ.

| Page | Contract |
| --- | --- |
| [Command line](interfaces/cli.md) | flags and slash commands |
| [Settings](interfaces/settings.md) | key, variable, default, range, scope |
| [Tool schemas](interfaces/tool-schemas.md) | each tool as a model is sent it |
| [Limits](interfaces/limits.md) | limits fixed in the binary |
| [Headless](interfaces/headless.md) | `-p`, `--json`, `--json-stream`, `--control` |
| [HTTP API](interfaces/http-api.md) | routes, the event stream, sockets |
| [On disk](interfaces/on-disk.md) | every file under `~/.uagent` |
| [Artifact formats](interfaces/artifact-formats.md) | what makes a page a deck, a document, a sheet or a design |

## Operations

| Page | Covers |
| --- | --- |
| [Install](operations/install.md) | installer, source build, services, updating, removing |
| [Container](operations/container.md) | the Compose appliance, its network rules and variables |
| [Remote access](operations/remote-access.md) | reverse proxy, pairing, push, SSH hosts |
| [Budgets and recovery](operations/budgets-and-recovery.md) | accounting, budgets, recovery, failure triage |

## Elsewhere

- [Manual](manual.md): the terminal and web clients, briefly.
- [Testing](testing.md): suites, sanitizers, browser tests, evaluation, CI.
- [Security](../SECURITY.md): threat model and boundaries.
- [Contributing](../CONTRIBUTING.md): build, conventions, release.
- [Benchmarks](../benchmarks/README.md): dated measurements and method.
