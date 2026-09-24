# System prompts

One native resolver builds the system prompt for model requests and serves
`/prompt`, the `uagent` tool's `inspect` topic `prompt`, the `adapt_system`
tool and the web editor (**Settings → System prompt**, or **System prompt** in
raw context inspection).

## Layers

Layers apply in order: built-in, global, project, conversation. Each layer is
in one of three modes:

| Mode | Effect |
| --- | --- |
| Inherit | Leaves earlier layers unchanged |
| Overlay | Appends one instruction block |
| Replace | Discards inherited behavioral text |

Editing inherited text starts a replacement prefilled with that text. An empty
replacement is valid; reset restores inheritance.

| Scope | Location |
| --- | --- |
| Global | `~/.uagent/system-prompt.json` |
| Project | `<working-directory>/.uagent/system-prompt.json` |
| Conversation | Saved with the conversation; copied by forks |

A document contains `{"mode":"overlay|replace","text":"..."}`. Each document
and the assembled system message are limited to 64 KiB. An invalid file is
reported and preserved, and the agent sends no model request until it is
repaired or reset.

Replacement removes the built-in behavioral and capability guidance. Runtime
capability facts and repository instructions are separate, labelled sources
that remain in the assembled message; memory is outside it. Tool schemas,
approvals, sandboxing and host limits are enforced by the runtime and cannot be
changed by prompt text.

## CLI

```sh
uagent --show-system-prompt
uagent --show-system-prompt --json
```

Inspection builds the configured tool registry without a model call. In an
active conversation, `/prompt show` also includes its overrides and actual tool
and repository context. Web previews without an active conversation are
labelled as base-prompt previews.

```text
/prompt show
/prompt edit --scope project
/prompt set --scope global --mode overlay --file instructions.md
/prompt set --scope project --mode replace --file "my prompt.md"
/prompt reset --scope conversation
```

`--scope` defaults to `conversation` and `set` defaults to `--mode overlay`.
`edit` opens `$VISUAL` or `$EDITOR` in the terminal and the shared editor in
the browser; a cancelled edit saves nothing.

Scripts use `uagent --control` with `kind=prompt`, `scope=global|project`,
`action=show|preview|set|edit|reset` and optional `cwd` to select a project.
Writes require the `revision` returned by `show`; `set` takes `text` and
`mode`, and `edit` takes one unique exact `old`/`new` replacement.
Conversation-scope changes go through the owning session.

## Agent control

With `UAGENT_ADAPT_SYSTEM=1`, the agent gets the `adapt_system` tool with
`action=show|set|edit|reset` and the same scopes. It must call `show` first and
pass the returned `revision` plus a non-empty `reason` with every write.
Global and project writes, replacements, and a first conversation `edit`
require mandatory human approval, including under YOLO. An approval is bound to
the exact proposed text and source revisions, expires after five minutes and
cannot be reused. Manual CLI and web saves are direct user actions and create
no approval request.

## Updates and inspection

Writes use atomic replacement and revision checks under the shared management
write lock; a stale save is rejected. Browser drafts persist in tab storage
across scope switches, refreshes and reopening the editor.

Changes apply to the next model request, including the next step of a running
turn, never to a request already in flight. Workers detect file changes by file
stamp at request boundaries, and the web views refresh through the event
stream. Revisions and timestamps never enter model-visible text, so unchanged
inputs produce identical system text. Inspection distinguishes the effective
next prompt from the last prompt sent by the current process; retained HTTP
captures cover earlier processes.

`UAGENT_PROMPT_OVERLAY` names an experimental JSON file whose named-section
`replace` entries and `append` text are applied to the built-in base before
scope resolution. Its digest is recorded in request traces; the scope editor
never rewrites it.
