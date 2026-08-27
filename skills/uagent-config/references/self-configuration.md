# Changing µAgent configuration

`uagent_configure` persists a setting. It accepts only registered `UAGENT_*`
names, shows the user an exact diff, and commits nothing until they approve it.
`--yolo` does not apply, and a headless or delegated run cannot commit at all,
so in those sessions the tool is not offered and the answer is a proposal for
the user to apply themselves.

Do not hand-edit `~/.uagent/.config` with the file tools. The tool merges into
the existing file and holds the approved bytes; a `write_file` over the same
path discards unrelated settings and skips the approval diff.

## Procedure

1. Read the current effective value and its source with `uagent_info`, topic
   `config`, `name` set to the exact setting. Report which layer currently
   wins: a command-line flag or process variable keeps shadowing a config file
   after it is edited.
2. Check the `takes_effect` field. Saying a change is live when it needs a
   restart is the failure this step exists to prevent.
3. Call `uagent_configure` with `scope` `user` for `~/.uagent/.config`, or
   `project` for `./.uagent/.config`, which requires a workspace the user has
   already trusted. Pass one entry per setting, each `set` with a value or
   `unset`.
4. State the effect plainly: what changes now, what changes at the next launch,
   and what stays shadowed by a higher layer.

Literal credentials are rejected by the tool. Composite settings such as
`UAGENT_PROVIDERS` may use exact environment-variable references; the approval
preview shows the reference and redacts any existing literal credential. Ask
the user to enter direct credentials, and never echo one that is already set.

## Layers

```text
command line          --model, --budget, --no-memory, ...
process environment   exported UAGENT_* variables
project config        ./.uagent/.config, only when trusted
user config           ~/.uagent/.config
built-in default      the registry default reported by uagent_info
```

`UAGENT_CONFIG_FILE` replaces both config-file layers.
