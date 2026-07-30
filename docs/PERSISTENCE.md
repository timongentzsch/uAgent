# Persistence

µAgent stores private, unencrypted local state. Records can contain prompts,
model output, tool results, paths, and usage.

| State | Location |
| --- | --- |
| sessions | `~/.uagent/history/<workspace>/*.json` |
| debug traces | `~/.uagent/sessions/*.jsonl` |
| process logs | `~/.uagent/bg/*`, `~/.uagent/terminals/*` |
| MCP logs | `~/.uagent/mcp/*` |
| captured large outputs | `~/.uagent/artifacts/*` |
| memories | `<base>/memory/*.md` |
| project trust | `~/.uagent/config/trusted-projects.json` |
| preferred model | `~/.uagent/config/model-preference.json` |

Session format 3 persists active messages, their structured kinds, the bounded
archive, checkpoints, usage, and provider session identity. Only the current
format is accepted: incompatible, incomplete, or corrupt records are reported
without mutating live state or rewriting the source file. Missing files are a
normal empty state. Saves validate the complete record, then use a private
same-directory temporary file, `fsync`, and atomic rename.

There is deliberately no implicit schema migration. A future format change
must ship an explicit, tested conversion or start a new session. Interrupted
writes leave the prior valid record intact; malformed files remain available
for diagnosis.

Completed process logs larger than `UAGENT_TOOL_RESULT_CHARS` move to the
private artifact directory instead of entering model context whole. Each is
bounded by `UAGENT_BASH_LOG_BYTES` and pruned during startup using
`UAGENT_BG_DAYS` and `UAGENT_BG_FILES`; a returned path can therefore expire.

Delete the corresponding files to remove local state. Rotate any credential
that appeared in a prompt, attachment, tool result, or debug trace. Sharing the
state directory can disclose repository content even though file permissions
are private.
