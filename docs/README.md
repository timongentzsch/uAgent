# Documentation

## Using µAgent

| Task | Guide |
| --- | --- |
| Install µAgent and connect a model | [Install and verify](operations/install.md) · [Models and providers](manual.md#models-and-providers) |
| Follow the same live turn from another device | [Pair a device](operations/remote-access.md#pair-a-device) · [Reach the host from a phone](operations/remote-access.md#give-the-host-its-public-origin) |
| Work in a folder on another machine | [Connect an SSH host](operations/remote-access.md#connect-an-ssh-execution-host) · [Where keys and data live](architecture/execution-gateway.md#responsibility) |
| Use Chrome profiles, saved logins and manual sign-in | [Browser controls](manual.md#the-agents-browser) · [Password handling](../SECURITY.md#browser-automation) |
| Create, edit and export workspace files | [Artifact workflow](manual.md#artifacts) · [Supported formats](interfaces/artifact-formats.md) |
| Delegate work or run it later | [Agents and to-dos](manual.md#agents-and-the-to-do-list) · [Memory and schedules](manual.md#memory-skills-and-schedules) |

[Full user manual](manual.md) · [Terminal shortcuts](manual.md#slash-commands-and-shortcuts)

## Architecture

Start with the [process diagram and source map](architecture/overview.md). The subsystem guides describe ownership, contracts, failure modes and tests.

| Subsystem | Guides |
| --- | --- |
| Model requests and tool execution | [Turn loop](architecture/turn-loop.md) · [Providers](architecture/providers.md) · [Tools and approvals](architecture/tools.md) · [Sandbox](architecture/sandbox.md) |
| Durable conversations | [Sessions](architecture/sessions.md) · [Memory and schedules](architecture/memory-and-schedules.md) |
| Devices and remote machines | [Web host](architecture/web-host.md) · [Web client](architecture/web-client.md) · [SSH execution gateway](architecture/execution-gateway.md) |
| Browser and file previews | [Chrome service](architecture/browser.md) · [Artifact rendering and editing](architecture/artifacts.md) |

## Interfaces

[CLI and commands](interfaces/cli.md) · [Settings](interfaces/settings.md) · [Tool schemas](interfaces/tool-schemas.md) · [Runtime limits](interfaces/limits.md)

[Headless JSON and streaming](interfaces/headless.md) · [HTTP API and sockets](interfaces/http-api.md) · [On-disk layout](interfaces/on-disk.md) · [Artifact formats](interfaces/artifact-formats.md)

## Operations

[Install and update](operations/install.md) · [Container](operations/container.md) · [Remote access](operations/remote-access.md) · [Budgets and recovery](operations/budgets-and-recovery.md)

## Development

[Build and contribute](../CONTRIBUTING.md) · [Tests and CI](testing.md) · [Benchmarks and raw results](../benchmarks/README.md) · [Security](../SECURITY.md)
