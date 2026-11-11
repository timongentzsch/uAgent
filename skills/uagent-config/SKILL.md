---
name: uagent-config
description: "µAgent itself: colours and fonts, settings, AGENTS.md, models, mode, commands"
---

# Configure µAgent

Answer from the running binary, never from memory: it is the installed
release, and it knows more than any copy of its documentation. Opening this
skill adds the `uagent` tool to the conversation. Its `action` is `show` (read
a `topic`), `set` (change settings), `write` (replace one whole file) or
`command` (run one slash command for the user); below, `show` `config` means
action `show` with topic `config`.

## Route the question

| Question | Source |
| --- | --- |
| What is active right now, and why? | `show` `status` or `config` |
| What does this setting default to, and when does a change apply? | `show` `config`, `name` set to its key or environment name |
| Which settings exist? | `show` `config`, no `name`: every setting with its key, environment name, type, default, purpose and where it is set |
| Which flags exist? | `uagent --help`, or `show` `cli` |
| Which slash commands exist? | `show` `commands`, or `/help` |
| What is in the system prompt? | `uagent --show-system-prompt` (`--json` adds the sources); how instructions layer onto it: `docs/INSTRUCTIONS.md` in a source checkout |
| Which prompt is this session actually running? | `show` `prompt` (base digest, active sections, and `saved`, the person's changes) |
| Which built-in tools and arguments exist? | `show` `tools` |
| Which model routes are configured, and is a credential set? | `show` `routes` |
| Which MCP servers are configured, and do they run? | `show` `mcp`: each server's file, state and tool count. `/mcp` shows the person why one failed |
| Which approvals are remembered for this folder? | `show` `permissions` |
| Which skills are installed, and which are on? | `show` `skills` |
| How is µAgent built, and why? | `references/architecture.md` when installed, else `docs/ARCHITECTURE.md` in a source checkout |
| Limits, recovery or failure triage | `docs/OPERATIONS.md` in a source checkout |
| Change a setting persistently | `set`; see `references/self-configuration.md` |
| What instructions do sessions read? | `show` `instructions` |
| Change instructions, the system message, MCP servers or a skill | `write`; see "Change a file" |
| Change the look of the web UI | `show` `style` for the current sheet, then `write` `style`; see "Change the look" |
| Change this conversation's model, effort, approval mode or tools, or compact it | `command`; see "Run a slash command" |

Do not load every reference; pick the one the question needs. Only
`self-configuration.md` and `architecture.md` are written by hand; every other
fact comes from the binary.

## Workflow

1. Establish scope: one run (a flag or an exported variable), all
   conversations (`user`), the conversations in this folder
   (`project`), or this conversation only (model and approval mode).
2. Read the live value before proposing a change; `show` reports the
   source of each active setting and whether a restart is required.
3. Persist a setting with `set`, which changes only the
   named settings in the saved document and shows the user the exact diff.
   Never print secret values; report only whether they are set.
4. Prefer scope `user` for persistent settings. Use `project` only when
   folder-specific behavior is intended; it is saved for that folder, not
   written into the repository, and needs no folder trust. Web host
   settings (`UAGENT_WEB_*`, `UAGENT_BROWSER_DATA`) save at `user` only. Use
   process exports for a one-off command. `set` has no conversation
   scope: that is `/config conversation KEY=VALUE`, `/model` or
   `/permissions`, which the user types or `command` runs for them.
5. Change only the settings needed for the requested outcome. Keep limits at
   defaults unless there is a measured reason to raise them.
6. Validate without a billable model call: `uagent --help`, `uagent --version`,
   and `show`. Explain that a real prompt is the end-to-end check and may
   incur provider usage.

## Change a file

`write` takes `file` and `text`, the whole new file: read what is there
first and keep what should stay. The user approves the exact diff, and the
write is refused when the file changed while they read it.

| `file` | What it is | Read it with | Applies |
| --- | --- | --- | --- |
| `instructions` | AGENTS.md for all conversations (`scope` `user`) or this folder (`project`) | `show` `instructions` | new and restarted sessions |
| `style` | the web UI's stylesheet; empty `text` resets the look | `show` `style` | open pages, when the turn ends |
| `prompt` | the person's changes to the system message, as JSON `{"replace": {"## Scratch": "..."}, "append": "..."}`: `replace` gives a section of the base new text (Evidence, Tools, Scratch, Changes, Delegation, Answer), `append` adds text after the last. Empty `text` restores the built-in message | `show` `prompt`, field `saved` | new and restarted sessions |
| `mcp` | this folder's `.mcp.json`, a JSON object with `mcpServers` | `read_path` on `.mcp.json`; the user approves the read, since the file can hold credentials | new and restarted sessions in this folder, once the user trusts the changed file |
| `skill` | the `SKILL.md` of one of the person's own skills, `~/.uagent/skills/NAME/SKILL.md`, with `name` its folder (letters, digits, `-`, `_`). It starts with YAML front matter that has a `description` | `show` `skills` for its path, then `read_path` | new and restarted sessions |

## Run a slash command

`command` takes `line`, one slash command as the user would type it:
`/model sonnet`, `/compact`, `/permissions auto`. The user approves the exact
line, in every approval mode. It does not run inside your turn: it is queued,
runs when the turn ends (after the messages the user queued for then), and
its output goes to the user, not to you. So ask for it last, say what you
asked for, and read the outcome with `show` in a later turn. A turn that is
interrupted or fails drops what it queued.

`show` `commands` lists the commands. Refused: one marked `client_only`
(navigation such as `/fork` or `/reset`, which the user does themselves), and
one that only starts a turn (`/init`, `/review`, `/diff`): do that work
yourself. An agent and a headless run are not offered `command`.

## Change the look

The web UI draws everything from design tokens, and the person's stylesheet
(`~/.uagent/config/style.css`, private to µAgent) overrides them. Only the
tokens are a stable surface; class names change between releases.

1. Read the current sheet with `show` `style`, and the tokens from
   `references/tokens.css` when installed (else
   `web/src/shared/styles/tokens.css` in a source checkout): the first
   `:root` block holds the scales (spacing, type, sizes, motion), then come
   a touch keyboard's overrides and, under "The two palettes", the dark
   palette (`:root`) and the light one (`:root[data-theme="light"]`).
2. Write only the tokens that change, and keep what the sheet already holds.
   The sheet outranks every token, so `:root { ... }` changes both palettes.
   For one palette use `:root[data-theme="dark"]` or `"light"`; to reach the
   inverse surface too (the person's own messages), copy that palette's whole
   selector list from the reference. Keep text readable: at least 4.5:1
   against its background.
3. `write` with `file` `style` and the whole sheet; the person
   approves the diff. Open web pages apply it when the turn ends. Empty text
   resets the look.

## Rules

- Apply precedence correctly: the conversation's own choice (model and
  approval mode only), then command-line flags, then process environment,
  then what is saved for the folder, then what is saved for all
  conversations, then built-in defaults.
- Treat provider keys (in `UAGENT_PROVIDERS` or the variables they name),
  web-search keys, and MCP credentials as secrets. `show` reports them as
  set or unset and never shows the value. A provider added through
  `set` names its key as `$VARIABLE`; the person sets the value
  in Settings or with `uagent setup`.
- Do not add secrets to a repository. Saved settings live in µAgent's own
  private `~/.uagent/config/settings.json`, never in a folder.
- `/model` and `/effort` choose for the conversation they are
  typed in only. `/model NAME --default` also saves the model for new
  conversations (`UAGENT_MODEL`).
- Do not enable `--yolo`, `UAGENT_APPROVAL=yolo`, folder trust, credential
  forwarding, or broader tool capabilities without making the authority change
  explicit. That holds for `command` with `/yolo`, `/permissions`, `/plan`
  or `/tools`: only when the user asked for that change.
- Do not invent settings. `show` `config` enumerates every registered
  setting; if a requested behavior is absent from it, say so rather than
  guessing.
- Never write the saved settings with the file tools; µAgent's config folder
  is closed to them. `set` and `write` are offered only
  in a session where a person can approve (a terminal or the web UI, not a
  headless run), and `command` only in the user's own conversation there.
  When the tool offers only `show`, give the user the
  `/config user KEY=VALUE` (or `project`) command to run themselves; do not
  make the change on their behalf.
