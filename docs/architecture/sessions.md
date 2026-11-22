# Sessions

This page explains what a conversation is on disk: the session file, the
files beside it, how each is written, and what survives a crash. Paths under
`~/.uagent` are listed in [On-disk formats](../interfaces/on-disk.md); the
runtime process that owns a session is in [Overview](overview.md).

## Responsibility

`SessionStore` (`src/agent/session_store.cc`) reads, writes, forks, renames
and removes session files. `SessionJournal` keeps a bounded event record
beside each, `EditJournal` keeps what file tools changed for undo, and
`src/core/mailbox.cc` carries mail between sessions. The web host moves a
conversation between folders (`src/app/session_move.cc`) and stores uploads
(`AssetStore`, `src/app/asset_store.cc`).

## Processes and threads

Only the session runtime that holds the writer lease writes a session
file: `Agent::Save` and `Agent::Load` take `<file>.lock` (`SessionLockPath`)
through `FileLease` before touching it. `SessionStore::Edit`, `Fork` and
`Remove` take the same lease, so a rename or delete from the web host waits
for, or fails against, a running runtime. Attached clients never take it.

The runtime saves when a user message enters the transcript, after each
input it handles (a turn, a rename, a control request), and at shutdown
(`Application::SaveSession`). Each save also flushes the journal; one whose
bytes match the last write is skipped (`SavedFile`).

## Data and formats

A session file lives in `history/` and holds two lines:

1. A header object under 16 KiB (`kSessionHeaderBytes`): `format`
   (`kSessionFormat`, 3), `cwd`, `model`, `session_id`, `turns`, `title`,
   `custom_title`, `parent_session_id`, `forked_at_turn`, `forked_at_time`;
   when set `host`, `agent`, `memory: false`, `suggestion`; and `incoming`,
   `cost`, `tokens`, `todo`, so a listing reads headers only.
2. The state object: `messages`, `message_kinds` (same length),
   `archive`, `archive_dropped_segments`, `context_tokens`,
   `context_window`, `usage`, `route_usage`, `display`
   and `delivered_mail`. `display` holds the facts the transcript view needs
   after messages are pruned or compacted.

The whole file is read with a 64 MiB cap (`kSessionReadBytes`); a save that
would exceed it is refused. A session has two identities: the hash of its
file name (`SessionIdFor`), which names its socket, its mailbox, its usage
ledger, the web's routes, pins and drafts, and agents' `parent`; and the
header `session_id` (`uagent-<hash>`), which a fork names as its source and
which keys the provider's prompt cache. Neither depends on the folder, and
no file is ever renamed, so both last the conversation's life.

Files beside `<name>.json`, all private:

| Suffix | Content |
| --- | --- |
| `.lock` | the writer lease |
| `.events.jsonl` | the event journal |
| `.edits/` | undo blobs, `index.json`, stored diffs |
| `.assets/` | uploads, each `<id>.data` with its record `<id>.json` |
| `.plan.md` | plan mode's plan |
| `<stem>.share.md` | `/share` export |

**Journal.** One line per durable event:
`{"schema":"uagent.session.event.v1","seq","time","type","data"}`. `data` is
a projection (`JournalProjection`) of turn, tool, activity, capability,
configuration and prompt metadata: names, outcomes, digests, sizes, usage.
It never holds prompts, answers, reasoning or tool output. At most 512
records and 256 KiB (`SessionJournal::kMaxEvents`, `kMaxBytes`); a flush
appends, and rewrites only after lines left the ring or a load.

**Archive.** At turn end the tool trace, and on compaction the replaced
messages, become archive segments (`{turn, reason, messages,
message_kinds, display_ids}`). The archive is capped at 16 MiB
(`kSessionArchiveBytes`); the oldest segments are dropped first and counted
in `archive_dropped_segments`. It is kept for display, not sent to the model.

**Undo.** `EditJournal::Record` stores the content a file had before a
turn's first change to it as `<turn>-<hash(path)>`, with the hash the turn
left. A file over 2 MiB, or past 64 MiB per session, is listed but not
undoable. `index.json` is written when the turn's summary lists the files
(at most 50) and after a revert. `Revert` restores a file only while it
still hashes to what the turn left ("changed since" otherwise). The
`changes` command draws a file's change in a turn with `git diff
--no-index` between the stored content and the file now; Files shows it
with Undo. Changes made by shell commands are not recorded.

**Assets.** The web host stores uploads: 8 MiB a file, 8 a message, 64 MiB
a session, 512 MiB in all (`asset_store.h`). A fork copies the directory
and rewrites paths, so no asset is shared between sessions.

**Mail.** `mail/<id>/new/` and `cur/`, `<id>` the session's id
(`SessionIdFor`). A message is one JSON file (`format` 1: `id`,
`from`, `to`, `sender_path`, `hops`, `created_ms`, `expires_ms`, `body`)
named `<created_ms>-<pid>-<seq>-<id>.json`, so sorting gives send order.

## Protocol

Saving: `SessionStore::Save` validates the state, serialises header and
body, and calls `WritePrivateFile`: a `mkstemp` file beside the target,
`fchmod` 0600, write, `fsync`, `close`, `rename`, then `fsync` of the
directory. The previous file stays whole until the rename.

Mail, sender to recipient:

1. `SendMail` refuses after 8 hops, over 64 KiB, with 64 messages pending,
   or past 20 sends a minute from one sender; a pending duplicate is
   dropped. It writes the file into `new/` atomically.
2. The recipient watches `new/` (inotify or kqueue). `TakeMail` deletes
   expired mail (24 h), moves unreadable files to `cur/*.bad`, checks that
   the sender may write to it, and renames what it takes into `cur/`.
3. `DeliverMail` adds the text to the conversation and remembers the id in
   `delivered_mail` (latest 256). After the next save, `AckMail` deletes the
   files from `cur/`.

A folder's conversations are those whose `cwd` (and `host`) is the
folder, with the agents whose `agent.parent` chain leads to one
(`SessionsIn`).

Moving a conversation (`SessionHost::MoveConversation`): the family is the
conversation and its agents. Busy members refuse the move; idle runtimes
are closed. Each file, agents first, gets the new `cwd` where it had the
old one, and the conversation a transcript note; the record the web host
kept of it as a draft goes. No file moves, so ids, paths, pins, mail, the
usage ledger, scheduled runs and side chats' link to it stay as they are.
The host publishes `moved` for each member, and a page reads it again.

## Invariants and limits

- Only format 3 loads: another is `kIncompatible`, a bad header or state
  `kCorrupt`, a foreign `cwd` or `host` `kWrongWorkspace`. None is
  rewritten or migrated.
- Mail enters a conversation at most once: an id in `delivered_mail` is
  acknowledged without being added again.

## Failure modes

- Crash mid-save: the temporary file is left beside the target and the
  last complete file loads.
- Torn journal: only a last line that fails to parse is dropped. A corrupt
  line elsewhere fails the load with a notice; the conversation still loads,
  since the journal is not replay authority.
- Crash after mail was taken but before a save: `RecoverMail` moves `cur/`
  back to `new/` at the next start, and the mail is delivered again.
- Activities, PTYs and output buffers do not survive the runtime, and a
  tool call that was running is not repeated. A detached terminal's record
  carries a boot-scoped process identity; one that does not match is
  treated as exited and never signalled.
- A move interrupted partway leaves some agents in the new folder and the
  conversation in the old one; moving it again finishes it.

Retention: once a day (`kMaintenanceIntervalHours`), a top-level runtime's
bootstrap calls `MaintainArtifacts`. Files in `history/` older than
`history.days` go, then the oldest beyond `history.files`; sidecars age by
their own mtime and do not count against the file limit. Orphaned journals,
and the socket and lease of a runtime that was killed, are removed. Log
trees follow `logs.keepDays` and `logs.keepFiles` (mail by age only); see
[Settings](../interfaces/settings.md).

## Source and tests

| Path | What |
| --- | --- |
| `include/agent/session_store.h`, `src/agent/session_store.cc`, `src/agent/conversation.cc` | format, save, load, fork, remove; archive |
| `src/agent/agent.cc`, `src/app/application.cc`, `src/app/commands_session.cc` | when a session is saved |
| `include/core/events.h`, `src/core/events.cc` | `SessionJournal` |
| `include/agent/edit_journal.h`, `src/agent/edit_journal.cc` | undo |
| `include/core/mailbox.h`, `src/core/mailbox.cc`, `src/agent/mail_delivery.cc` | mail |
| `include/app/asset_store.h`, `src/app/asset_store.cc` | uploads and quotas |
| `src/app/session_move.cc` | moves |
| `src/core/fs.cc` | `AtomicWriteFile`, `MaintainArtifacts` |
| `tests/unit/session_command_test.cc`, `tests/unit/events_test.cc` | save, load, fork, rename; journal |
| `tests/unit/activity_sessions_test.cc`, `tests/unit/filesystem_test.cc` | mail; undo, atomic writes |
| `tests/integration_runtime/sessions.py` | runtime start, resume |
| `tests/integration_web/moves.py` | moving a conversation |
