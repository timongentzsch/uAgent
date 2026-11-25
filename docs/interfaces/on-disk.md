# On-disk layout

Every file and directory µAgent writes or reads by name: `~/.uagent`
(`GlobalBase()`), the runtime directory under `/tmp`, a project folder and
a remote host. Formats with a page of their own are linked. Directories
µAgent creates are 0700 and its files 0600 (`kPrivateDirMode`,
`kPrivateFileMode`) unless a row says otherwise.

## Per-user state: `~/.uagent`

With no home directory (`HOME`, then the password entry) it is
`/tmp/uagent-<uid>/.uagent`.

| Path | Format | Written by | Read by |
| --- | --- | --- | --- |
| `~/.uagent/history/<name>.json` | session file, format 3 ([Sessions](../architecture/sessions.md)) | the runtime holding the lease | runtimes, the web host, `-c` |
| `~/.uagent/history/<name>.json.{lock,events.jsonl,edits/,assets/,plan.md}`, `<stem>.share.md` | sidecars ([Sessions](../architecture/sessions.md#data-and-formats)) | the runtime; uploads by the web host | the same |
| `~/.uagent/mail/<id>/{new,cur}/*.json` | one message per file ([Sessions](../architecture/sessions.md#data-and-formats)) | the sender, the recipient | the recipient |
| `~/.uagent/memory/` | Markdown memories, `events.jsonl`, `.processed/` ([Memory and schedules](../architecture/memory-and-schedules.md)) | the `memory` tool, the extractor, `--control` | every runtime |
| `~/.uagent/skills/<name>/SKILL.md` | skills | the Library, import, the `uagent` tool | every runtime |
| `~/.uagent/AGENTS.md` | your instructions; `AGENTS.override.md` wins, `CLAUDE.md` is read when `skills.otherAgents` names claude | you, `instructions` control | every runtime |
| `~/.uagent/scheduled/state.json`, `write.lock` | schedules and runs ([Memory and schedules](../architecture/memory-and-schedules.md)) | `--control`, the web host's scheduler | the same |
| `~/.uagent/library/changed`, `write.lock`; `staging/<name>.XXXXXX/` | change stamp and lease for memories and skills; a skill being imported (pruned after a day) | Library writers, import | watchers, import |
| `~/.uagent/worktrees/<id>/` | git worktrees of agents and scheduled runs | agent and schedule launch | those runs |
| `~/.uagent/spaces/<name>/`, `spaces/<name>/.uagent/name` | named workspaces (`Scratch` for a conversation of no folder) and the display name | the web host | the web host |
| `~/.uagent/artifacts/{output,exchange,http}-XXXXXX` | large tool output; tool exchanges and HTTP captures (`logs.level` full and above) | runtimes | `read`, `/http`, the web host |
| `~/.uagent/activity/<pid>-<activity>.log`, `pending-*`, `usage-<session id>.jsonl` | command logs; a child agent's spend, one JSON object per line | the shell tool, child agents | the activity tool, the parent |
| `~/.uagent/terminals/<pid>.json`, `terminals/logs/` | detached terminal records and logs | the shell tool | the activity tool |
| `~/.uagent/sessions/<UTC stamp>-<pid>.jsonl`, `mcp/<server>-<pid>.log` | trace (sensitive, `logs.level` debug or `--debug`); MCP server stderr (`logs.level` events and above) | runtimes | people |
| `~/.uagent/models/<hash>.json` | a provider's model catalogue, no credentials | runtimes | runtimes |
| `~/.uagent/openrouter-signin.json`, `.maintained` | a pending sign-in's verifier; the daily retention stamp | `setup`, `openrouter` control; runtimes | the same |

`<name>` is `<UTC stamp>-uagent-<hash>` (terminal,
headless), `web-<32 hex>`, `agent-<16 hex>`, `fork-uagent-<hash>` or
`scheduled-<id>`; unique across folders, since the name is the id. Retention is
in [Sessions](../architecture/sessions.md#failure-modes).

## Settings and configuration: `~/.uagent/config`

| Path | Format | Written by | Read by |
| --- | --- | --- | --- |
| `settings.json`, `settings.json.lock` | the settings document (below) | `/config`, the web settings, `setup`, the `uagent` tool | every process |
| `settings.schema.json` | its JSON Schema, rewritten when the build changes | any reader | editors |
| `trusted-projects.json` | folders whose `.mcp.json` is trusted | the trust prompt | runtimes |
| `permissions.json` | `{version, rules: [{root, tool, match, effect, created}]}`, `effect` `allow`, `ask` or `deny` | an approval's Always or Never, `/permissions allow\|ask\|deny`, Settings › This folder › Rules | the approver, `permission_rules` |
| `prompt.json` | changes to the system message's sections | `instructions` control, `uagent` tool | runtimes |
| `style.css` | the person's stylesheet for the web client | the `uagent` tool, you | `GET /api/user.css` |
| `update.json` | the last release check | the web host | the web host |
| `welcomed` | marker: the first-run note was shown | the terminal client | the terminal client |

`settings.json` is written under its `.lock` by `ChangeSettings`. `all`
holds settings for every conversation; `projects` maps a workspace key
(folder path, or `ssh:<host id>:<path>`) to that folder's. Keys are the
flat dotted names of [Settings](settings.md); `variables` holds text a value
may reference as `$NAME`. The conversation scope lives in the session file.

```json
{
  "$schema": "./settings.schema.json",
  "format": 3,
  "all": {"limits.maxToolCalls": 120, "variables": {"MY_KEY": "…"}},
  "projects": {"/home/me/repo": {"approval.mode": "auto"}}
}
```

## Web host: `~/.uagent/web` ([Web host](../architecture/web-host.md#data-and-formats))

| Path | Content |
| --- | --- |
| `master.lock`, `discovery.json`, `devices.json` | single-instance lease; `v`, `port`, `url`, `secret`, `epoch`; devices: `id`, `token`, `name`, `expires` |
| `vapid.pem`, `push-subscriptions.json` | Web Push key and subscriptions |
| `pins.json`, `drafts/<session id>.json` | pins; titles of conversations not yet saved |
| `hosts.json`, `client-<host id>.json`, `ssh-<host id>.log`, `remote-terminals.json` | saved SSH hosts, the client identity per host, the last SSH log, remote shells |

## Runtime directory: `/tmp/uagent-<euid>-<hash>`

`RuntimeDir()`; `<hash>` hashes `GlobalBase()`. It must be the user's, 0700.

| Path | Content |
| --- | --- |
| `<session id>.sock`, `<session id>.sock.lock` | a runtime's socket and writer lease |
| `<32 hex>.json`, `<32 hex>.json.error` | launch file (deleted on read), boot error |
| `remote/<first 12 of host id>.ctl` | SSH control sockets of saved hosts |
| `e-<hash>.sock`, `e-<hash>.sock.lock` | local end of an execution forward |
| `exec/<hash>/{socket,owner,state/}` | an execution worker's socket, lease and state ([Execution gateway](../architecture/execution-gateway.md#data-and-formats)) |

## A project folder

| Path | Written | Read |
| --- | --- | --- |
| `.mcp.json` | by the `uagent` tool (`mcp`), 0600 when new | when trusted; `~/.mcp.json` always |
| `AGENTS.override.md`, `AGENTS.md`, `CLAUDE.md` | `AGENTS.md` by `instructions` control and the `uagent` tool, 0644 | the first present per directory, from the repository root down; `CLAUDE.md` only when `skills.otherAgents` names claude |
| `.uagent/skills/`, `.agents/skills/` | by you | as skills; no setting is read from a folder |
| `.uagent/scratch/` | scripts the agent writes, with a `.gitignore` of `*` | by the agent |

## A remote host

`~/.uagent/remote/builds/<build id>/` holds an installed build (`umask
077`), removed after 30 days unused (`kRemoteBuildDays`). A worker keeps its
state under the remote's runtime directory (`exec/`, above); conversations,
settings and memory stay on the main.

## Errors

A file that does not parse is reported and left as it is: the settings
document (the error names it, nothing is saved over it), a session file
(`kIncompatible`, `kCorrupt`). A private directory that is a symlink, not
the user's, or open to others is refused (`EnsurePrivateDirectory`).

## Compatibility

Versioned files: `settings.json` and session files (`format` 3), the
journal (`uagent.session.event.v1`), mail files (`format` 1),
`permissions.json` (`version` 3), and each folder's record in
`trusted-projects.json` (`format` 3). Only the current version is read and
nothing is migrated; a `permissions.json` of another version is refused
with a message naming it, and no rule of it applies until it is removed; a
trust record of another format trusts nothing, so the folder's `.mcp.json`
needs trusting again. Paths not listed here are not contract.

## Source and tests

| Path | What |
| --- | --- |
| `include/core/fs.h`, `src/core/fs.cc` | directory names, `GlobalBase`, `RuntimeDir`, private writes, retention |
| `include/core/settings_store.h`, `src/core/settings_store.cc` | the settings document |
| `src/core/private_store.cc`, `src/app/permissions.cc`, `src/core/config.cc`, `src/web/master.cc`, `src/web/remote_hosts.cc`, `src/app/launch.cc`, `src/app/agent_tools.cc`, `src/app/session_server.cc` | config stores, web directory, session names, launch files |
| `tests/unit/filesystem_test.cc`, `tests/integration_tools.py` | private writes; saved settings |
