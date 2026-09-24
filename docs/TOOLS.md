# Tools

The tools µAgent offers the model, when each is available, and how calls are
approved. The exact schemas and their sizes are in the generated
[tool reference](../skills/uagent-config/references/tools.md); `/context` shows
the set a live session currently advertises, including MCP tools.

## Inventory

The registry is built at startup and refreshed when MCP tool lists change. It is
filtered by the toolset (`UAGENT_TOOLSET=lean` withholds implementation tools),
`UAGENT_TOOL_CAPABILITIES` (`inspect`, `execute`, `mutate`, `delegate`,
`external`), route capabilities, installed skills, delegation depth and
per-conversation choices made with `/tools`.

| Tool | Purpose | Available |
| --- | --- | --- |
| `read_path` | read text or line ranges, list directories, load images and documents | always |
| `grep` | regex or literal search of file contents or paths | always |
| `write_file` | create a file; replacing one needs `overwrite=true` | full toolset |
| `edit_file` | apply ordered exact replacements atomically | full toolset |
| `delete_file` | delete a regular file and show the removed content | full toolset |
| `run` | run a supervised shell command, optionally with a PTY or detached | always |
| `scratch` | write and rerun one `.py` (under uv) or `.sh` script in `.uagent/scratch` | `uv` or `python3` on `PATH` |
| `activity` | list, poll, wait for, write to, resize or stop activities | when activities exist |
| `memory` | list, search and read memory; write when the user asks | full toolset, memory enabled |
| `uagent` | inspect this build (status, flags, commands, config, tools, prompt, routes); change settings | full toolset |
| `web_fetch` | read one public http(s) URL as text | always |
| `web_search` | cited web search through OpenRouter's hosted search | an OpenRouter-protocol route or search endpoint |
| `session` | list linked sessions and message them | always |
| `subagent` | delegate a subtask to a durable collaborator | delegation depth below `UAGENT_SUBAGENT_DEPTH` |
| `skill` | load an installed skill | a usable skill is installed |
| `adapt_system` | read and revise the system prompt | `UAGENT_ADAPT_SYSTEM=1`; see [SYSTEM_PROMPTS.md](SYSTEM_PROMPTS.md) |
| `browser` | drive the shared Chrome of the browser appliance | top-level web sessions with `UAGENT_BROWSER_DATA`; see [WEB.md](WEB.md) |
| `<server>_<tool>` | tools discovered from MCP servers; see [OPERATIONS.md](OPERATIONS.md#mcp) | configured servers; not in lean children |

Independent calls to parallel-safe tools may run concurrently; results are
appended in the order the model issued the calls. A tool with a per-turn cap
(`web_search` 4, `subagent` 32 by default) is withdrawn for the rest of the
turn once the cap is reached. Tool limits, timeouts and turn budgets apply in
every approval mode.

## Approval

Paths outside the workspace need approval. Mutating, process and network tools
follow the permission mode (`/permissions`, `UAGENT_APPROVAL`):

- **Ask** shows the full action and can allow it once, for the session, or
  always for that exact action in this repository. A remembered rule covers
  the tool's provider, schema, approval class and arguments, so a change to
  any of them asks again. Rules live in `~/.uagent/config/permissions.json`
  and can be removed from the web Settings page.
- **Auto** sends the user request and a bounded preview of the action to
  OpenRouter's Decisions API (`UAGENT_PERMISSION_MODEL`, default
  `~typesafe/jev-latest`; `UAGENT_PERMISSION_URL`) and follows its allow, ask
  or deny answer. An ask opens the normal prompt, or denies when no
  interactive client is attached; network, authentication and parse failures
  are treated the same way. Reviewer usage counts toward the turn and
  session.
- **YOLO** (`--yolo`, `/yolo`) approves ordinary mutations and turns the
  command sandbox off.

Some actions always need a person: reading or writing µAgent's config files,
`.mcp.json` or `permissions.json`, writing the project trust store or a
`system-prompt.json`, changing settings through `uagent`, and
`run(sandbox=false)`. Remembered rules and automatic modes do not apply, and a
session with nobody to ask denies. Child processes get the sanitized
environment described in [SECURITY.md](../SECURITY.md).

## Files and search

- `read_path` loads images, documents and binary files through the same
  capability-aware pipeline as attachments; omit line ranges for media.
- `write_file` publishes atomically. Without `overwrite=true` it refuses any
  existing path, including a symlink; use `edit_file` for targeted changes.
- `grep` defaults to regex content matches. `literal=true` searches exact
  text, `mode=files` matches file names and `mode=matching_files` returns the
  paths whose contents match; both path modes ignore `context`.
- A byte-identical repeat of a `read_path` or `grep` result still in recent
  context is returned as a short receipt.
- `run`, `scratch` and `grep` execute inside the OS sandbox: writes are
  limited to the workspace, temporary directories and package caches; reads
  and, by default, the network stay open. See
  [SECURITY.md](../SECURITY.md) for the full policy.

## Activities

`run` waits `UAGENT_RUN_YIELD_MS` (10 s) and then returns a still-running
command as an activity. `yield_ms` of 250–30,000 overrides the wait and `0`
waits until the command exits. Set `tty=true` only when the process needs
interactive input; a PTY keeps merged output, writable input, interruption and
resize. `detach=true` keeps the command running after the session ends, with a
rotating log.

Every `activity` call names one `operation`:

| Operation | Arguments |
| --- | --- |
| `list` | |
| `poll` | `id`; optional `wait_ms` and `until` (a readiness marker) |
| `wait` | `wait_ms`; optional `ids` (default: all) and `mode=any\|all` |
| `write` | `id`, `chars`; an empty string is a valid write and `\u0003` interrupts the process group |
| `resize` | `id`, `rows` and `cols` in 1..1000; PTY activities only |
| `stop` | `id`; ends the process group and removes its records and logs |

`run` and `activity` accept `max_output_chars` (256–65,536) to lower the output
cap for one call. Output returned once is not returned again. Writing to a
non-PTY activity is rejected.

## Delegation

`subagent` defaults to `operation=spawn` and returns an activity ID and a
durable collaborator ID.

- `followup` resumes the collaborator's private conversation and prepends its
  stored `directive`; an empty directive clears it.
- `message` delivers one-shot guidance at the child's next step, or at the next
  follow-up when it is idle.
- `list` reports this workspace's collaborators with model, toolset and state.
- `persistent=true` keeps one blocking collaborator's session worker and
  processes alive between handoffs; its model, mode and limits are fixed at
  spawn. Stop it through `subagent`.
- Use `activity` to wait for, read or stop ordinary children. `/agents` shows
  the same records.

## Web

`web_fetch` needs no hosted-search route. It decodes HTML, JSON, XML and plain
text and refuses other content types; download PDFs and images with `run` and
open them with `read_path`. Bodies over `UAGENT_WEB_FETCH_BYTES` are truncated
and marked partial. Pages behind a login or built by scripts need the browser.

Only public Internet addresses are accepted. Every resolved IPv4 and IPv6
address of the initial request and of each redirect is checked; loopback,
private, carrier-grade NAT, link-local, reserved and multicast addresses are
refused, including IPv4-mapped, NAT64, Teredo and 6to4 forms. Proxy variables
are ignored so a proxy cannot resolve an unchecked address.

`web_search` has one schema for every model. It calls OpenRouter's hosted
`openrouter:web_search` on its own route, so citations and accounting do not
depend on the conversation model. `UAGENT_WEB_SEARCH_BACKEND=off` withholds it.

## Presentation

Each call is recorded as `explore`, `change` or `execute`. Native tools use
their own contract; `run` and `scratch` take an optional `intent` (default
`execute`) that labels the activity and never changes permissions. Adjacent
successful exploration in one batch folds into one row in the web UI and a
compact terminal summary; changes, failures and approval prompts stay visible.
`/trace` and `/verbose` show full detail in the terminal.
