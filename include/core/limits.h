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
inline constexpr size_t kMiB = size_t{1024} * 1024;

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

// Bounded collections retained in session metadata or exposed through UI
// catalogues. These are memory and payload policies, not inferred capacities.
inline constexpr size_t kMaxSkillFiles = 128;
inline constexpr int kMaxSkillFileDepth = 4;
inline constexpr size_t kLibraryNameChars = 100;
inline constexpr size_t kMaxSessionAssets = 64;
inline constexpr size_t kMaxCollaboratorRecords = 100;
inline constexpr size_t kMaxToolDisplays = 128;
inline constexpr size_t kMaxToolsPerMessage = 32;
inline constexpr size_t kMaxActiveExchanges = 32;
inline constexpr size_t kMaxPendingSessionCommands = 32;
inline constexpr size_t kMaxAdaptiveProposals = 16;
// Tool overrides survive optional MCP registries being absent during startup,
// so bound both their persisted count and key size before retaining unknown
// names for a later registry refresh.
inline constexpr size_t kMaxToolSelectionOverrides = 256;
inline constexpr size_t kToolNameChars = 128;

// Fixed policies with one owner. They are named because changing them alters
// observable behavior, even though they do not need runtime configuration.
inline constexpr size_t kToolDedupeMinChars = 256;
inline constexpr size_t kGenericTitleReplacementMinChars = 12;
// Upload and subsequent claim validation must accept the same display names.
inline constexpr size_t kAssetNameChars = 128;
inline constexpr size_t kImageAnalysisCacheEntries = 8;
inline constexpr size_t kModelPickerMatches = 256;
inline constexpr size_t kConfigurationChangeLimit = 64;
inline constexpr size_t kSharedToolResultChars = 2000;
inline constexpr size_t kRetainedArtifactPathChars = 4096;
inline constexpr size_t kKqueueWatchTargets = 64;
inline constexpr int64_t kModelRequestDeadlineReserveSeconds = 1;

// Live event pacing. Model callbacks and browser publication use the same
// usage cadence so one layer cannot silently undo the other's coalescing.
inline constexpr int64_t kUsageProgressIntervalMs = 100;
// A quiet provider chunk still advances context promptly even when it arrives
// inside the time interval and no later chunk wakes the progress callback.
inline constexpr size_t kUsageProgressBytes = KiB(1);
inline constexpr int64_t kStreamBatchIntervalMs = 12;
inline constexpr size_t kStreamBatchBytes = KiB(8);

// Turn-loop recovery stages. A valid repeated call gets two model-facing
// corrections before the high ceiling prevents an unbounded paid loop.
// Deterministically rejected calls stop sooner because rerunning them cannot
// produce new evidence without changing the request.
inline constexpr int64_t kRepeatedCallAdviseAfter = 3;
inline constexpr int64_t kRepeatedCallDirectAfter = 6;
inline constexpr int64_t kRepeatedCallStopAfter = 12;
inline constexpr int64_t kRejectedCallStopAfter = 3;
inline constexpr int64_t kFailedToolAdviseAfter = 3;
inline constexpr int64_t kRepeatedToolRoundTraceAfter = 8;
inline constexpr int64_t kActivityPollAdviseAfter = 2;
inline constexpr int64_t kActivityPollDirectAfter = 4;
inline constexpr int64_t kActivityPollStopAfter = 12;

// Time-unit sentinels shared by schedule validation, cache headers and
// display formatters. A year here is 365 days (31536000s), matching the
// HTTP `max-age` convention, not a leap-corrected calendar year.
inline constexpr int64_t kSecondsPerMinute = 60;
inline constexpr int64_t kSecondsPerHour = 3600;
inline constexpr int64_t kSecondsPerDay = 86400;
inline constexpr int64_t kSecondsPerYear = 31536000;
inline constexpr int64_t kMillisecondsPerSecond = 1000;
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
