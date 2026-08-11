# Persistence

µAgent stores private, unencrypted local state. Records can contain prompts,
model output, tool results, paths, and usage.

| State | Location |
| --- | --- |
| sessions | `~/.uagent/history/<workspace>/*.json` |
| debug traces | `~/.uagent/sessions/*.jsonl` |
| process logs | `~/.uagent/bg/*`, `~/.uagent/terminals/*` |
| MCP logs and captured images | `~/.uagent/mcp/*` |
| captured large outputs | `~/.uagent/artifacts/*` |
| memories | `~/.uagent/memory/{global,projects/<repository>}/*.md` |
| memory extraction state | `~/.uagent/history/<workspace>/*.json.memory` |
| one-off Python scratch scripts | `<workspace>/.uagent/scratch/*.py` |
| project trust | `~/.uagent/config/trusted-projects.json` |
| preferred model | `~/.uagent/config/model-preference.json` |
| certified route profiles | `~/.uagent/config/routes.json` |

Session format 3 persists active messages, their structured kinds, the bounded
archive, checkpoints, usage, and provider session identity. Only the current
format is accepted: incompatible, incomplete, or corrupt records are reported
without mutating live state or rewriting the source file. Missing files are a
normal empty state. Saves validate the complete record, then use a private
same-directory temporary file, `fsync`, and atomic rename.

Route profiles are evaluator-generated, versioned, and atomically replaced.
Runtime reads only the active route entry; full reports and fixture traces stay
at the paths selected with `uagent eval --report` and `--artifacts`.
Profiles use schema 3; detailed reports independently use schema 2. Only an
explicit `uagent eval --certify` updates profiles, after every selected trial
passes. Profiles record the exact scenario sample counts and expire after 30
days.

There is deliberately no implicit schema migration. A future format change
must ship an explicit, tested conversion or start a new session. Interrupted
writes leave the prior valid record intact; malformed files remain available
for diagnosis.

Memory generation reuses saved sessions rather than introducing another
transcript store. A neighboring `.json.memory` file atomically claims a source;
`done` records successful consolidation. Interrupted claims become retryable
after 24 hours, and a session modified by resume is eligible again after its
configured idle window. The child sees a bounded tail containing only user,
assistant, tool-result, and attachment text; system, project, memory,
runtime/internal, reasoning, and binary attachment data are excluded.
Individual project/global memory files remain the only recall source.

Completed process logs larger than `UAGENT_TOOL_RESULT_CHARS` move to the
private artifact directory instead of entering model context whole. Each is
bounded by `UAGENT_BASH_LOG_BYTES` and pruned during startup using
`UAGENT_BG_DAYS` and `UAGENT_BG_FILES`; a returned path can therefore expire.

Completed turn traces enter the bounded archive before old bulky tool results
are compacted in active model context. The placeholder preserves protocol and a
short preview; the archive and optional debug log retain the original while
their configured retention permits. Repeated source reads still hit the
filesystem; only byte-identical results with a recent visible original are
deduplicated before the next model request.

Delete the corresponding files and memory marker sidecars to remove local
state. Rotate any credential
that appeared in a prompt, attachment, tool result, or debug trace. Sharing the
state directory can disclose repository content even though file permissions
are private.
