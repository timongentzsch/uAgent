// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LIMITS_H_
#define UAGENT_INCLUDE_CORE_LIMITS_H_
// Compile-time policy constants that several modules must agree on. Nothing
// here is tunable: runtime-adjustable values belong in the EnvBounded/EnvStr
// table in src/core/env.cc and in RuntimeConfig. Keep this header dependency
// free -- it is included widely.

#include <cstddef>
#include <cstdint>

namespace uagent {

// Byte multiples. Prefer these over bare `* 1024` chains so a budget reads
// as a budget and a second spelling (registry::kMb) cannot drift.
inline constexpr size_t kKiB = 1024;
inline constexpr size_t kMiB = 1024 * 1024;

inline constexpr size_t KiB(size_t n) { return n * kKiB; }
inline constexpr size_t MiB(size_t n) { return n * kMiB; }

// Distinct preview/trace budgets that previously shared bare 4096/512.
// A trace preview, a catalogue header read and a retained-fact cap are
// different policies: growing one must not silently grow the others.
inline constexpr size_t kPreviewChars = 4096;
inline constexpr size_t kCatalogueHeaderBytes = 4096;
inline constexpr size_t kTraceSummaryChars = 512;
// Retained change/diff preview on a tool-result fact: larger than the trace
// summary because a diff is the whole row, but still bounded.
inline constexpr size_t kChangePreviewChars = size_t{16} * 1024;
// Decoded-image cache cap and largest dimension kept for vision payloads.
inline constexpr size_t kImageCacheBytes = size_t{16} * 1024 * 1024;
inline constexpr int kMaxImageDimension = 2048;
// Kept-receipt detail cap: compact rows record title/summary/flags, and only
// multiline rows carry a bounded detail body.
inline constexpr size_t kReplayDetailChars = 2048;
// Terminal columns reserved beside a record title when truncating its summary.
inline constexpr size_t kRecordTitleReserveChars = 6;
// Maximum entries held in a session/asset catalogue scan. Distinct from the
// byte budgets above: raising retention must not raise preview windows.
inline constexpr size_t kMaxCatalogueEntries = 4096;

// Time-unit sentinels shared by schedule validation, cache headers and
// display formatters. A year here is 365 days (31536000s), matching the
// HTTP `max-age` convention, not a leap-corrected calendar year.
inline constexpr int64_t kSecondsPerMinute = 60;
inline constexpr int64_t kSecondsPerHour = 3600;
inline constexpr int64_t kSecondsPerDay = 86400;
inline constexpr int64_t kSecondsPerYear = 31536000;
// Latest epoch accepted by schedule validation (2100-01-01T00:00:00Z).
// Named so the three schedule.cc checks and any future caller agree.
inline constexpr int64_t kMaxScheduleEpoch = 4102444800LL;

// Hash-fingerprint widths for stable short digests. The occurrence/detail
// pair must stay in sync between request construction and view projection.
inline constexpr size_t kDigestChars = 12;
inline constexpr size_t kOccurrenceChars = 16;
inline constexpr size_t kDetailChars = 24;
inline constexpr size_t kAgentNameChars = 8;

// Adaptive-prompt budgets shared by session persistence, the CLI editor and
// the adapt_system tool. A receipt larger than this is truncated before it
// is stored, so all three must agree.
inline constexpr size_t kAdaptiveSystemBytes = size_t{64} * 1024;
inline constexpr size_t kAdaptiveSystemReasonBytes = 512;
// Memory-event journal budgets: the append-only audit log is compacted past
// this size, and a single event line beyond this is refused. The writer, the
// readers and the retention policy must agree.
inline constexpr size_t kMemoryEventJournalBytes = size_t{256} * 1024;
inline constexpr size_t kMemoryEventLineBytes = 4096;

// Agent-private artifacts (sessions, logs, memories, config). Owner-only, so a
// shared machine cannot read transcripts or credentials out of ~/.uagent.
inline constexpr int kPrivateFileMode = 0600;
inline constexpr int kPrivateDirMode = 0700;

// Files written into the user's workspace on their behalf, which follow the
// usual world-readable convention rather than the private-artifact policy.
inline constexpr int kSharedFileMode = 0644;

// Bounds for the `run` tool's initial yield. The lower bound keeps a yield from
// degenerating into a busy poll; the upper bound keeps one call from consuming
// the turn. The tool schema, the argument validator, the clamp in the shell
// runner and the UAGENT_RUN_YIELD_MS env bound must agree.
inline constexpr int64_t kMinYieldMs = 250;
inline constexpr int64_t kMaxYieldMs = 30000;

// Settle time for an activity write or resize that named no wait_ms: long
// enough for the child to echo, short enough not to feel like a wait.
inline constexpr int64_t kActivityInputSettleMs = 250;

// Background slots held back from delegated children. Subagents, foreground
// commands and background jobs share the UAGENT_MAX_BACKGROUND_JOBS pool
// (detached terminals are counted separately), so an unbounded fan-out of
// children would leave the parent unable to build, test or search -- the very
// work it needs in order to check what those children produced. The guarantee
// only bites once the pool exceeds this headroom.
inline constexpr int64_t kDelegatedJobHeadroom = 2;

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_LIMITS_H_
