# System prompts

One native resolver serves model requests, `/prompt`, `uagent_info topic=prompt`,
the `adapt_system` tool and the web editor. Open **Settings → System prompt**
or use the **System prompt** button in raw context inspection.

Layers apply in order: built-in, global, project, conversation. **Inherit**
leaves earlier layers unchanged; **Overlay** adds one instruction block;
**Replace** discards inherited behavioral text. Editing changes the selected
layer. Editing inherited text starts a replacement prefilled from that text.
An empty replacement is valid; reset is a separate operation that restores
inheritance.

Global documents live in `~/.uagent/system-prompt.json`; project documents in
`<working-directory>/.uagent/system-prompt.json`. A document contains
`{"mode":"overlay|replace","text":"..."}`. Conversation overrides are saved
with the conversation and copied by forks. Each document and the assembled
system message are bounded to 64 KiB. Invalid files are reported and preserved;
the agent stops before sending a model request until they are repaired.

Replacement removes built-in behavioral and capability guidance. Runtime
capability facts and repository instructions are separate, labelled context
sources and remain in the assembled system message. Memory remains outside it.
Tool schemas, approvals, sandboxing and host limits are enforced by the runtime
and cannot be changed by prompt text.

## CLI and agent controls

```sh
uagent --show-system-prompt
uagent --show-system-prompt --json
```

Inspection builds the configured tool registry without a model call. An active
conversation's `/prompt show` also includes its overrides and actual tool and
repository context. Standalone web management previews without an active
conversation are explicitly labelled as base-prompt previews.

```text
/prompt show
/prompt edit --scope project
/prompt set --scope global --mode overlay --file instructions.md
/prompt set --scope project --mode replace --file "my prompt.md"
/prompt reset --scope conversation
```

`edit` opens `$VISUAL` or `$EDITOR` in the terminal and the shared editor in the
browser. Failed editor processes leave the prompt unchanged. Native scripts
can use `--control` with `kind=prompt`, `scope=global|project`, and
`action=show|preview|set|edit|reset`. Writes require the revision returned by
`show`; `set` takes `text` and `mode`, and `edit` takes one unique exact `old` /
`new` replacement. Use `cwd` to select a project. Conversation controls use the
owning session, preserving its single-writer rule.

With `UAGENT_ADAPT_SYSTEM=1`, the agent can use the same operations through
`adapt_system`. Read with `action=show` first, then include the returned
revision and a `reason` in the write. Persistent writes and full replacements
use mandatory human approval, including under YOLO. The preview is bound to
the proposed bytes and source revisions and cannot be reused. Legacy
`instructions` / `reason` calls still replace the conversation overlay;
an empty instruction clears it. Manual CLI and web saves are direct user
actions and do not create an agent approval request.

## Updates and inspection

Writes use atomic replacement and revision checks under the shared management
write lease. A stale save is rejected. Browser drafts remain in tab storage;
scope switching, event refreshes and reopening the editor preserve them.

`prompt.changed` events describe conversation changes; persistent file changes
also refresh management views through the existing event stream. Workers check
cached file stamps at request boundaries. Changes apply to the next model
request, including the next step of a running turn, never to an in-flight
request. Revisions and timestamps are not inserted into model-visible text.
Unchanged inputs produce identical system text. Inspection distinguishes the
effective next prompt from the last prompt sent in the current process;
retained HTTP captures remain the source for previous-process requests.

The experimental `UAGENT_PROMPT_OVERLAY` retains its named-section replacements
and append behavior, applied to the built-in base before scope resolution.
Experiment digests remain in request traces. The scope editor does not rewrite
experiment files.
