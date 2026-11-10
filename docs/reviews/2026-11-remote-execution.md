# Remote execution review

Source audit of [PR #336](https://github.com/timongentzsch/uAgent/pull/336),
2026-11-10. GPT-6 Astra reviewed production commit `015ce3a1` against base
`9cdbb553`, including callers, ownership and existing regression coverage.
The initial review was read-only. **All five findings are now addressed** by
the follow-up cleanup described below. Original source references and line
counts refer to the audited revision; this is not a certification of the
entire repository.
No new correctness blocker was established by this review.

## Scope and size

Physical added/deleted lines from `git diff --numstat 9cdbb553 015ce3a1`:

| Area | Added | Deleted | Net |
| --- | ---: | ---: | ---: |
| Native production (`src/`, `include/`) | 3,827 | 432 | +3,395 |
| Web production (`web/src/`) | 2,031 | 440 | +1,591 |
| Tests (`tests/`, `web/tests/`) | 1,313 | 51 | +1,262 |
| Build, packaging and CI | 399 | 35 | +364 |
| Vendored code/documentation | 46 | 14 | +32 |
| Documentation | 372 | 2 | +370 |
| Other | 1 | 1 | 0 |

These counts include comments and whitespace; they are not executable SLOC.
They describe the audited production revision, before the documentation and
benchmark-reporting follow-up. No speculative line-saving target is assigned
to a refactor before implementing and measuring it.

## Findings

### P2: unused HTTP relay extensions — resolved

The audited revision added `ranges_applied` and streamed-upload arguments to
`open_stream` in [httplib.h](../../third_party/cpp-httplib/httplib.h), around lines 1762,
2763, 9649, 13972 and 14885. No production or test caller used these extensions;
internal library callers used the original arguments. The vendor README also
named `TestRemoteProxyStreamsAndKeepsCredentialsHostOwned`, which is absent.

Removed only the relay-specific vendor extensions and obsolete documentation.
The socket liveness analyzer annotation remains as the only vendor change.
File ranges, previews and terminals keep the established HTTP implementation.

### P2: shared file-service extraction — resolved

[workspace_files.cc](../../src/app/workspace_files.cc), lines 319–394, and
[master_files.cc](../../src/web/master_files.cc), lines 152–237 and 265–298,
both orchestrated file operations, changes, revisions and writes. The shared
`Request` handler had one production caller, in `execution.cc`; Local HTTP
reconstructed the dispatch and results from lower-level helpers.

Local and remote JSON operations now use `workspace_files::Request`, including
reads, writes, revisions and changes. `Master` owns authentication, HTTP status,
headers, CSP and rendering; Local downloads retain a direct byte path without
hex serialization. Listing, search and Git-change implementation details are
private; `Candidates` and `Shown` remain available to their Library callers.
A transport-parity integration test compares reads, listings, search, changes,
diffs, missing files, truncation, containment and revision conflicts.

### P3: unreachable SSH host state — resolved

In [remote_hosts.cc](../../src/web/remote_hosts.cc), lines 32–39, 320–329,
345, 387 and 463, `failed` was reset but never set true, `generation` was
incremented but never read, and `socket` was assigned/unlinked but never
opened or forwarded. Actual execution sockets belong to `execution::Client`.

Removed those fields, the unreachable retry branch, its wake condition and
unused socket cleanup/include. Request, attempt and endpoint state, child
monitoring, retry timing and explicit disconnect behavior remain intact.

### P3: obsolete multiple-stream modes — resolved

[use-host.ts](../../web/src/state/use-host.ts), lines 104–107, 139–154,
437–440 and 699–706, retained enable/active modes, per-host stores and remote
authentication/push branches from the multiple-stream design. Its sole caller,
`app/hosts.tsx:44`, used the defaults above the contextual API provider; the
hook always owned the main catalogue and stream.

The hook now uses the main API explicitly and one main-owned store. Removed
the unused enable/active arguments, per-host store map and remote authentication
and push branches. Browser visibility handling and conversation-host navigation
remain; feature-specific host-qualified storage is unchanged. Main store keys
retain their existing values so reloads preserve drafts, unread state and receipts.

### P3: repeated archive validation — resolved

[remote_hosts.cc](../../src/web/remote_hosts.cc), lines 137–158, repeated
manifest checks and a selected-archive checksum immediately after
`CheckRemoteBundle` had validated the build and all four archive hashes.

`CheckRemoteBundle` now returns the validated manifest. Installation reuses its
selected record instead of reading and hashing it again. All four local payloads
must still match the build, size and hash. Remote transfer verification and
installer verification after copying remain separate checks. A regression test
covers complete bundles, wrong builds/target sets, and each missing or corrupt
payload.

## Retained boundaries and evidence limits

Main-owned provider/configuration/history state, execution-only remote
processes, build-keyed installation reuse and receipts for uncertain mutations
have concrete ownership and recovery purposes. The four native packaging legs
and archive integrity checks are necessary portability/integrity work, not
automatic deletion candidates.

The audit inspected source, diffs, references and existing test coverage; it
did not independently rerun provider inference, deployment or CI. The separate
[measured snapshot](../../benchmarks/results/2026-11-10.json) records package
sizes, local benchmarks and the exact successful CI run. Documentation-only
updates after that revision are not new runtime performance measurements.

## Cleanup scope

Against documentation revision `79a91b14`, the cleanup removes **176 net
first-party production lines** (`src/`, `include/`, `web/src/`) and **25 net
vendor-header lines**. These are physical added/deleted lines, including comments
and whitespace. The two integration regressions add 84 test lines; documentation
is counted separately. Dated benchmark/package measurements remain tied to
`015ce3a1`; they are not relabeled as measurements of this cleanup.

Local validation: the debug build and all 11 CTest suites pass, including 195
core tests, 345 integration tests and the behavioral evaluation. One Linux-only
integration test is skipped on macOS. All 121 frontend unit tests, TypeScript
checks, Biome lint and repository lint pass. Browser and hosted CI results are
tracked in the PR so their status is not confused with this source review.

## Second review and follow-up

Claude Code **Opus 5.5, high effort** reviewed `0fc24e65` against `9cdbb553`,
including the in-progress sidebar and bundle-validator edits. A second pass
read every changed frontend diff (95 files). These were source reviews; the
reviewer did not run tests or inspect private configurations. SVG bodies,
Firefox rendering and iOS standalone navigation were not runtime-validated.

The backend follow-up addresses all six actionable findings:

- Removed the unreachable Unix-socket web-host/update mode, its admission
  locks and idle-update endpoint. Restored the bounded request-pool sizing.
- Resolve `web.remoteBundle` through user configuration with environment
  precedence. The integration regression installs from a saved setting and
  from an environment override with an intentionally invalid saved path.
- Build remote file-write RPCs from the public path/text/revision fields.
  Browser-supplied IDs cannot become internal receipt identities; repeated
  extra IDs are covered by successive writes through HTTP.
- Disable Reconnect while connected or connecting; the frontend checks both.
- Correct stale panel names, execution-liveness comments and review/vendor
  provenance statements.
- Preserve historical competitor-version provenance alongside the fresh,
  separately versioned comparison.

Optional cleanup removes the obsolete discovery-pairing queue and redundant
Local guards in the remote-only panel. The worker removes its empty temporary
remote working directory on normal exit; `rmdir` does not delete contents.
A crashed process can still leave a directory until runtime-directory cleanup.

The frontend addendum identified ownership and interaction issues: Add host
submission, notification/session host routing, unknown-host fallback,
reconnect admission, draft ownership, repeated pin scans, folder suggestions,
Local-only UI clutter, selection copy, concurrent connection controls, and
API-test coverage. The follow-up centralizes their shared behavior instead of
adding separate desktop/mobile implementations. Validation is recorded below.


### Follow-up validation

- Debug native build and all native CTest groups passed; the web group was
  run separately with **93 passing integration cases**.
- **121 frontend unit tests** and **61 focused Chromium/WebKit tests** passed,
  including new connection, routing, draft and folder-selection regressions.
- Render checks use **1,000 conversations** and verify that an activity tick
  updates only the affected conversation row.
- Type checks, production web build, formatting, lint and source contracts
  passed. The comparison's 20 headless turns all succeeded; each idle session
  settled, and cleanup left no measured tool processes running.

This follow-up removes **32 net first-party production lines** relative to
`0fc24e65` (physical lines under `src/`, `include/` and `web/src/`). Tests,
documentation and benchmark reporting are counted separately. Hosted CI and
production deployment remain separate gates; these local results do not claim
a deployment or live remote-host retest of the follow-up.
