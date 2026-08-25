# Changing µAgent configuration

µAgent has no tool that writes its own configuration. Persisting a setting is a
file edit the user must approve, and the agent's role is to propose it
precisely.

## Procedure

1. Read the current effective value and its source with `uagent_info`, topic
   `config`, `name` set to the exact setting. Report which layer currently
   wins: a command-line flag or process variable keeps shadowing a config file
   after it is edited.
2. Confirm the setting exists and that its `takes_effect` field says whether
   the change applies at the next user turn or needs a restart.
3. Read the target file and preserve every unrelated line, comment and blank
   line. `~/.uagent/.config` is the user layer; `./.uagent/.config` applies
   only in a workspace the user has already trusted.
4. Propose the smallest edit that achieves the outcome, and state the effect
   plainly: what changes now, what changes at the next launch, and what stays
   shadowed.
5. Never write a secret through a tool argument. Ask the user to enter
   credentials themselves, and never echo an existing one.

## Layers

```text
command line          --model, --budget, --no-memory, ...
process environment   exported UAGENT_* variables
project config        ./.uagent/.config, only when trusted
user config           ~/.uagent/.config
built-in default      the registry default reported by uagent_info
```

`UAGENT_CONFIG_FILE` replaces both config-file layers.

## What the agent must not do

- Grant project trust. Trust is a user decision made with
  `--trust-project-config` after reviewing the workspace.
- Enable `--yolo` or `UAGENT_APPROVAL=yolo` to make its own work easier.
- Widen `UAGENT_TOOL_CAPABILITIES`, raise budgets, or disable memory redaction
  without saying that authority is being changed.
- Claim a change is active when the reference says it needs a restart.
