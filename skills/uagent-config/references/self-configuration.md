# Changing µAgent configuration

`uagent` with `action=configure` persists a setting. It accepts only
registered setting names, shows the user an exact diff, and commits nothing
until they approve it. `--yolo` does not apply. An approved preview is good
for five minutes. A headless or delegated run cannot commit at all: `inspect`
still works, `configure` is rejected, and the answer is a proposal for the
user to apply themselves with `/config user KEY=VALUE` or
`/config project KEY=VALUE`.

Do not edit `~/.uagent/config/settings.json` with the file tools. The tool
changes only the settings it was approved for and is refused if one of them
changed since the preview; a `write_file` over the document discards whatever
else was saved meanwhile and skips the approval.

## Procedure

1. Read the current effective value and its source with `uagent` action `inspect`, topic
   `config`, `name` set to the exact setting. Report which layer currently
   wins: a command-line flag, a process variable or the conversation's own
   choice keeps shadowing a saved setting after it is changed.
2. Check the `takes_effect` field. Saying a change is live when it needs a
   restart is the failure this step exists to prevent.
3. Call `uagent` with `action=configure` and `scope` `user` for all
   conversations, or `project` for the conversations in this folder. Pass
   `changes` with one entry per setting: `key`, `operation` (`set` or
   `unset`) and, for `set`, `value` as a string. Web host settings
   (`UAGENT_WEB_*`, `UAGENT_BROWSER_DATA`) are refused at `project`.
4. State the effect plainly: what changes now, what changes at the next launch,
   and what stays shadowed by a higher layer.

Literal credentials are rejected by the tool. Composite settings such as
`UAGENT_PROVIDERS` may use exact environment-variable references; the approval
preview shows the reference and redacts any existing literal credential. Ask
the user to enter direct credentials, and never echo one that is already set.

## Layers

```text
this conversation     its own model and approval mode (/model, /permissions,
                      /config conversation KEY=VALUE); not set by this tool
command line          --model, --budget, --no-memory, ...
process environment   exported UAGENT_* variables
this project          saved for this folder
all conversations     saved for every conversation
built-in default      the registry default reported by uagent
```

Both saved layers are one document, `~/.uagent/config/settings.json`;
`uagent config export` prints it and `uagent config import FILE` replaces it.
