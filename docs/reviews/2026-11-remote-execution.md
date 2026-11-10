# Remote execution review

Source audit of [PR #336](https://github.com/timongentzsch/uAgent/pull/336),
2026-11-10. GPT-6 Astra reviewed production commit `015ce3a1` against base
`9cdbb553`, including callers, ownership and existing regression coverage.
This is a read-only review: the findings below are **open cleanup candidates**,
not implemented refactors or a certification of the entire repository.
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

### P2: remove unused HTTP relay extensions from the vendor fork

The PR adds `ranges_applied` and streamed-upload arguments to `open_stream`
in [httplib.h](../../third_party/cpp-httplib/httplib.h), around lines 1762,
2763, 9649, 13972 and 14885. No production or test caller uses these extensions;
internal library callers use the original arguments. The vendor README also
names `TestRemoteProxyStreamsAndKeepsCredentialsHostOwned`, which is absent.

Revert only the PR's relay-specific vendor changes and their obsolete
documentation, preserving prior vendor fixes. This removes a maintenance
obligation left by the superseded HTTP-proxy design. Validate native compilation
and existing file/range/preview and terminal coverage.

### P2: complete the shared file-service extraction

[workspace_files.cc](../../src/app/workspace_files.cc), lines 319–394, and
[master_files.cc](../../src/web/master_files.cc), lines 152–237 and 265–298,
both orchestrate file operations, changes, revisions and writes. The shared
`Request` handler has one production caller, in `execution.cc`; Local HTTP
still reconstructs the dispatch and results from lower-level helpers.

Route Local JSON operations through the same service while keeping HTTP
authentication, response status/headers, CSP and rendering in `Master`.
Binary downloads should retain a direct byte path rather than acquiring hex
serialization merely to reuse dispatch. Make helpers private only after checking
their remaining callers: `Candidates` and `Shown` also serve the Library.
Validate equivalent Local/remote reads, missing files, write conflicts,
truncation and containment through `integration_web/files.py` and `remote.py`.

### P3: delete unreachable SSH host state

In [remote_hosts.cc](../../src/web/remote_hosts.cc), lines 32–39, 320–329,
345, 387 and 463, `failed` is reset but never set true, `generation` is
incremented but never read, and `socket` is assigned/unlinked but never
opened or forwarded. Actual execution sockets belong to `execution::Client`.

Remove the dead fields, unreachable reconnect branch, unused wake condition
and socket cleanup. Keep the live request/attempt/endpoint state and child
monitoring. Existing authentication-failure, manual-reconnect and
disconnect/recovery cases exercise the relevant behavior.

### P3: remove obsolete multiple-stream modes from the main hook

[use-host.ts](../../web/src/state/use-host.ts), lines 104–107, 139–154,
437–440 and 699–706, retains enable/active modes, per-host stores and remote
authentication/push branches from the multiple-stream design. Its sole caller,
`app/hosts.tsx:44`, uses the defaults above the contextual API provider; the
hook always owns the main catalogue and stream.

Make that ownership explicit and remove unused mode branches. Preserve real
document-visibility behavior and host-qualified drafts/storage in their feature
consumers. Validate host navigation, reload/reconnect, unread/title, notification
and outgoing-command behavior.

### P3: reuse the selected archive's validation result

[remote_hosts.cc](../../src/web/remote_hosts.cc), lines 137–158, repeats
manifest checks and a selected-archive checksum immediately after
`CheckRemoteBundle` has validated the build and all four archive hashes.

Reuse that validation result, preferably returning the validated manifest or
record from the existing validator. Keep remote verification of transferred
bytes and installer verification after copying: those protect different
boundaries. Validate missing/malformed bundles and package contents. This
does not justify a generalized validation framework.

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
