# Persistence

µAgent stores private, unencrypted local state. Records can contain prompts,
model output, tool results, paths, and usage.

| State | Location |
| --- | --- |
| sessions | `~/.uagent/history/<workspace>/*.json` |
| bounded session journals | `<session>.json.events.jsonl` |
| debug traces | `~/.uagent/sessions/*.jsonl` |
| native memory audit | `~/.uagent/memory/events.jsonl` (bounded metadata and redacted previews) |
| process logs | `~/.uagent/bg/*`, `~/.uagent/terminals/logs/*` (the latter is not age-pruned; a live terminal writes there) |
| detached terminal records | `~/.uagent/terminals/<pid>.json` |
| session-lifetime activity state | memory only; not resumable after process exit |
| collaborator metadata and sessions | `~/.uagent/collaborators/{agent-id}.json` and `.session.json` |
| MCP logs and captured images | `~/.uagent/mcp/*` |
| Playwright snapshots and logs | `<workspace>/.playwright-cli/*` |
| captured large outputs | `~/.uagent/artifacts/*` |
| memories | `~/.uagent/memory/{global,projects/<repository>}/*.md` |
| processed memory-extraction claims | `~/.uagent/memory/.processed/*` |
| one-off Python scratch scripts | `<workspace>/.uagent/scratch/*.py` |
| project trust | `~/.uagent/config/trusted-projects.json` |
| preferred model | `~/.uagent/config/model-preference.json` |

Session format 3 persists active messages, structured kinds, the bounded
archive, usage, provider session identity, and the mutable system directive and
revision. A private `uagent.session.event.v1` JSONL sidecar retains at most 512
lifecycle records or 256 KiB. It contains bounded turn, tool, capability,
configuration, presentation, and artifact metadata—never prompts, reasoning or
answer deltas, or full tool results. Corrupt or absent journal data does not
prevent the format-3 conversation from loading; the journal is neither replay
authority nor model context.

The session snapshot retains tool calls and results used to rebuild the visible
timeline. Successful `show_image` entries are retransmitted from their recorded
paths; image bytes are not stored. Missing or invalid paths are skipped safely.
Only the current format is accepted: incompatible, incomplete, or corrupt
records are reported without changing live state or the source file. Missing
files are a normal empty state. Saves validate the complete record, then use a
private same-directory temporary file, `fsync`, and atomic rename.

There is deliberately no implicit schema migration. A future format change
must ship an explicit, tested conversion or start a new session. Interrupted
writes leave the prior valid record intact; malformed files remain available
for diagnosis.

Collaborators have durable logical identities even though each follow-up is a
new supervised child process. Their private metadata records the originating
workspace and route options; the adjacent atomic session snapshot is the
conversation authority. Queued messages persist in metadata until a follow-up
launch succeeds. Metadata and session files are pruned together at startup by
the debug-retention bounds (`UAGENT_DEBUG_DAYS` and `UAGENT_DEBUG_FILES`). Live
activity ownership, PTY state, and incremental buffers remain process-local and
are not reconstructed after a coordinator exits.

µAgent deliberately does not add a second canonical "rollout" log. The atomic
session snapshot is replay authority, the bounded journal is operational
metadata, and `--debug` is the opt-in sensitive reconstructable trace. Keeping
those contracts separate avoids claiming crash recovery from a truncated event
tail or retaining prompts by default.

Native project/global files are writable through an explicitly requested
`memory` action or the single bounded background extractor. Top-level Codex
memory files under `~/.codex/memories` and the current Claude project files
under `~/.claude/projects/<project>/memory` are read-only recall sources.
The extractor marks a source `done` only after a successful run. A failed or
interrupted child releases its `processing` claim immediately so a later run
can retry; an untrappable process death still falls back to stale-claim expiry.

Memory Markdown files remain the authoritative editable content. The bounded
private event audit stores only action, key, timestamp, automatic source, and a
redacted preview so `/memory` can explain when and why an entry changed. Each
automatic extractor also uses a private per-activity receipt that is removed
after its parent renders the maintenance notification.

Completed logs larger than `UAGENT_TOOL_RESULT_CHARS` move to the private
artifact directory instead of entering model context whole. Each is bounded by
`UAGENT_BASH_LOG_BYTES` and pruned at startup according to `UAGENT_BG_DAYS` and
`UAGENT_BG_FILES`, so returned paths can expire.
Session-lifetime supervised activities have opaque IDs, bounded incremental
head/tail output, and optional PTY state only in memory. They cannot be resumed
after µAgent exits. Persistent detached records remain PID-backed, rotating-log
based, and noninteractive; no broker retains their stdin. Each new record also
stores a boot-scoped kernel start identity. A missing or mismatched identity is
treated as exited and is never signalled, so legacy records fail closed rather
than risking a reused PID.

Completed turn traces enter the bounded archive before old bulky tool results
are compacted in active model context. The placeholder preserves protocol and a
short preview; the archive and optional debug log retain the original while
their configured retention permits. Repeated source reads still hit the
filesystem; only byte-identical results with a recent visible original are
deduplicated before the next model request.

Session journals share snapshot retention, do not count independently against
the history file ceiling, and are removed when their owning snapshot expires.

Delete the corresponding files to remove local state. Rotate any credential
that appeared in a prompt, attachment, tool result, or debug trace. Sharing the
state directory can disclose repository content even though file permissions
are private.
