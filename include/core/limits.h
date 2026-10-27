// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LIMITS_H_
#define UAGENT_INCLUDE_CORE_LIMITS_H_
// Compile-time policy constants. Nothing here is tunable: user-facing settings
// belong in include/core/config_registry.h. Keep this header dependency free
// -- it is included widely.

#include <cstddef>
#include <cstdint>

namespace uagent {

// Byte multiples. Prefer these over bare `* 1024` chains so a budget reads
// as a budget and a second spelling (registry::kMb) cannot drift.
inline constexpr size_t kKiB = 1024;
inline constexpr size_t kMiB = size_t{1024} * 1024;

inline constexpr size_t KiB(size_t n) { return n * kKiB; }
inline constexpr size_t MiB(size_t n) { return n * kMiB; }

// A trace preview, a catalogue header read and a retained-fact cap are
// different policies with their own budgets: growing one must not silently
// grow the others.
inline constexpr size_t kPreviewChars = 4096;
inline constexpr size_t kCatalogueHeaderBytes = 4096;
// The opening of a change/diff that a tool-result fact and its live event
// carry. A longer diff is kept whole in a private artifact the row names.
inline constexpr size_t kChangePreviewChars = KiB(16);
// Cap of the encoded attachments kept for the model calls of a turn, and the
// largest dimension kept for vision payloads.
inline constexpr size_t kAttachmentCacheBytes = MiB(16);
inline constexpr int kMaxImageDimension = 2048;
// Maximum entries held in a session/asset catalogue scan. Distinct from the
// byte budgets above: raising retention must not raise preview windows.
inline constexpr size_t kMaxCatalogueEntries = 4096;

// Bounded collections retained in session metadata or exposed through UI
// catalogues. These are memory and payload policies, not inferred capacities.
inline constexpr size_t kMaxSkillFiles = 128;
inline constexpr int kMaxSkillFileDepth = 4;
inline constexpr size_t kLibraryNameChars = 100;
inline constexpr size_t kMaxSessionAssets = 64;
inline constexpr size_t kMaxAgentRecords = 100;
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
// Generated session titles: first-message excerpt sent, title kept, and the
// deadline after which the first line stays.
inline constexpr size_t kTitleInputChars = 2000;
inline constexpr size_t kTitleChars = 80;
// The longest title a user (or a fork) may give a session, and the longest
// workspace name, in characters; a generated title is cut to as many bytes,
// which is never more characters. The web form mirrors both
// (web/src/shared/limits.ts).
inline constexpr size_t kUserTitleChars = 256;
inline constexpr size_t kSpaceNameChars = 64;
inline constexpr int64_t kTitleTimeoutSeconds = 30;
// Enough for a title once the model does not think first.
inline constexpr int64_t kTitleMaxTokens = 64;
// Upload and subsequent claim validation must accept the same display names.
inline constexpr size_t kAssetNameChars = 128;
inline constexpr size_t kImageAnalysisCacheEntries = 8;
inline constexpr size_t kModelPickerMatches = 256;
inline constexpr size_t kConfigurationChangeLimit = 64;
inline constexpr size_t kSharedToolResultChars = 2000;
inline constexpr size_t kRetainedArtifactPathChars = 4096;
inline constexpr size_t kKqueueWatchTargets = 64;
inline constexpr int64_t kModelRequestDeadlineReserveSeconds = 1;
// What the round that answers for a turn stopped at a limit may take, also
// past the turn's own deadline.
inline constexpr int64_t kLimitAnswerSeconds = 60;
// Parallel workers for one batch of parallel-safe tool calls.
inline constexpr int64_t kToolConcurrency = 4;

// Live event pacing. Model callbacks and browser publication use the same
// usage cadence so one layer cannot silently undo the other's coalescing.
inline constexpr int64_t kUsageProgressIntervalMs = 100;
// A quiet provider chunk still advances context promptly even when it arrives
// inside the time interval and no later chunk wakes the progress callback.
inline constexpr size_t kUsageProgressBytes = KiB(1);
inline constexpr int64_t kStreamBatchIntervalMs = 12;
inline constexpr size_t kStreamBatchBytes = KiB(8);
// Short activity captions are presentation policies, not model limits.
inline constexpr size_t kActivityLabelBytes = 160;
// A background job's full command, kept with its row and its result.
inline constexpr size_t kActivityCommandBytes = KiB(8);
inline constexpr size_t kActivityLineBytes = 192;

// Turn-loop recovery stages. A valid repeated call gets two model-facing
// corrections before the high ceiling prevents an unbounded paid loop.
// Deterministically rejected calls stop sooner because rerunning them cannot
// produce new evidence without changing the request.
inline constexpr int64_t kRepeatedCallAdviseAfter = 3;
inline constexpr int64_t kRepeatedCallDirectAfter = 6;
inline constexpr int64_t kRepeatedCallStopAfter = 12;
inline constexpr int64_t kRejectedCallStopAfter = 3;
inline constexpr int64_t kFailedToolAdviseAfter = 3;
inline constexpr int64_t kActivityPollAdviseAfter = 2;
inline constexpr int64_t kActivityPollDirectAfter = 4;
inline constexpr int64_t kActivityPollStopAfter = 12;

// Time-unit sentinels shared by schedule validation, cache headers and
// display formatters. A year here is 365 days (31536000s), matching the
// HTTP `max-age` convention, not a leap-corrected calendar year.
inline constexpr int64_t kSecondsPerMinute = 60;
inline constexpr int64_t kSecondsPerYear = 31536000;
inline constexpr int64_t kMillisecondsPerSecond = 1000;
// Latest epoch accepted by schedule validation (2100-01-01T00:00:00Z).
// Named so the three schedule.cc checks and any future caller agree.
inline constexpr int64_t kMaxScheduleEpoch = 4102444800LL;

// Compaction budgets. The summarizer reads up to kCompactionTranscriptBytes of
// transcript; a route whose window is under kCompactionSmallWindowTokens gets
// twice its window in bytes, never below the floor. The skills and the user
// messages carried over the summary are capped, and shrink with the window
// down to kCompactionRouteFloorBytes.
inline constexpr size_t kCompactionTranscriptBytes = KiB(256);
inline constexpr size_t kCompactionTranscriptFloorBytes = KiB(16);
inline constexpr int64_t kCompactionSmallWindowTokens = int64_t{128} * 1024;
inline constexpr size_t kCompactionSkillBytes = KiB(32);
inline constexpr size_t kCompactionUserBytes = KiB(80);
inline constexpr size_t kCompactionRouteFloorBytes = KiB(4);

// Polling. A poll tick re-checks something that has no event to wake on (a
// socket that is not bound yet, a lock another process holds); a wait tick
// bounds a blocking wait so an abort or a spinner is noticed.
inline constexpr int kPollTickMs = 20;
inline constexpr int kWaitTickMs = 100;

// Lifetimes shared by more than one site. A proposal (an adaptive prompt, a
// settings change) waits this long for the user's decision. An upload or an
// import that nothing claimed is an orphan after a day. A host with no wake
// due sleeps a day.
inline constexpr int64_t kProposalLifetimeMinutes = 5;
inline constexpr int64_t kOrphanAgeHours = 24;
inline constexpr int64_t kIdleSleepHours = 24;

// Hash-fingerprint widths for stable short digests. The occurrence/detail
// pair must stay in sync between request construction and view projection.
inline constexpr size_t kDigestChars = 12;
inline constexpr size_t kOccurrenceChars = 16;
inline constexpr size_t kDetailChars = 24;
inline constexpr size_t kAgentNameChars = 8;

// The assembled system message, and what an external editor may return.
inline constexpr size_t kSystemPromptBytes = KiB(64);
// Memory-event journal budgets: the append-only audit log is compacted past
// this size, and a single event line beyond this is refused. The writer, the
// readers and the retention policy must agree.
inline constexpr size_t kMemoryEventJournalBytes = KiB(256);
inline constexpr size_t kMemoryEventLineBytes = 4096;

// Agent-private artifacts (sessions, logs, memories, config). Owner-only, so a
// shared machine cannot read transcripts or credentials out of ~/.uagent.
inline constexpr int kPrivateFileMode = 0600;
inline constexpr int kPrivateDirMode = 0700;
// The group and other permission bits: set on a private file, they mean
// someone else can reach it.
inline constexpr int kGroupOtherModeBits = 0077;

// Files written into the user's workspace on their behalf, which follow the
// usual world-readable convention rather than the private-artifact policy.
inline constexpr int kSharedFileMode = 0644;

// Bounds on tool arguments. The schemas the model sees are built from these,
// and the policy check enforces the schema, so they are stated once.
inline constexpr int64_t kToolTextArgChars = 65536;
inline constexpr int64_t kToolMinOutputChars = 256;
inline constexpr int64_t kToolArgChars = 4096;
inline constexpr int64_t kPtyMaxDimension = 1000;

// Bounds for the `run` tool's initial yield. The lower bound keeps a yield from
// degenerating into a busy poll; the upper bound keeps one call from consuming
// the turn. The tool schema is built from them, so the argument validator and
// the clamp in the shell runner agree with it.
inline constexpr int64_t kMinYieldMs = 250;
inline constexpr int64_t kMaxYieldMs = 30000;
inline constexpr int64_t kDefaultYieldMs = 10000;

// Settle time for an activity write or resize that named no wait_ms: long
// enough for the child to echo, short enough not to feel like a wait.
inline constexpr int64_t kActivityInputSettleMs = 250;

// Concurrent supervised activities in one session.
inline constexpr int64_t kMaxBackgroundJobs = 8;
// Background slots held back from delegated children. Subagents, foreground
// commands and background jobs share the kMaxBackgroundJobs pool (detached
// terminals are counted separately), so an unbounded fan-out of children would
// leave the parent unable to build, test or search -- the very work it needs
// in order to check what those children produced.
inline constexpr int64_t kDelegatedJobHeadroom = 2;
inline constexpr int64_t kMaxDelegatedJobs =
    kMaxBackgroundJobs - kDelegatedJobHeadroom;

// Resource ceilings. They bound runaway input and disk use; none is a
// preference a user should have to tune.
// Model requests and the tool trace.
inline constexpr size_t kRequestBytes = MiB(64);
inline constexpr size_t kResponseBytes = MiB(32);
inline constexpr size_t kToolTraceProtectChars = KiB(64);
inline constexpr size_t kToolTracePruneMinChars = KiB(32);
inline constexpr int64_t kSessionArchiveBytes = MiB(16);
// Built-in tools. read_path's result allowance covers its header lines.
inline constexpr int64_t kReadFileMaxLines = 10000;
inline constexpr size_t kReadFileBytes = KiB(32);
inline constexpr int64_t kReadFileResultChars = kReadFileBytes + 2048;
inline constexpr size_t kEditFileBytes = MiB(10);
inline constexpr int64_t kListDirEntries = 1000;
inline constexpr int64_t kListDirScanEntries = 100000;
inline constexpr int64_t kGrepResults = 200;
inline constexpr int64_t kBashLogBytes = MiB(64);
// The web host's file browser: a file's first MiB shows as text, a search
// answers this many paths.
inline constexpr size_t kFileViewBytes = MiB(1);
inline constexpr size_t kFileFindMatches = 200;
// Delegation and hosted search.
inline constexpr int64_t kSubagentCallsPerTurn = 32;
inline constexpr int64_t kWebSearchTimeoutSeconds = 60;
inline constexpr int64_t kWebSearchMaxTokens = 2000;
inline constexpr int64_t kWebSearchCalls = 4;
inline constexpr int64_t kWebSearchMaxResults = 5;
inline constexpr int64_t kWebSearchMaxUses = 3;
// Memory, skills, project instructions and attachments.
inline constexpr size_t kMemoryAlwaysBytes = 2048;
inline constexpr size_t kMemoryBytes = 2048;
inline constexpr size_t kMaxMemories = 32;
inline constexpr size_t kMemoryExtractBytes = KiB(32);
// Idle time before a saved session is offered to background extraction.
inline constexpr int64_t kMemoryIdleSeconds = int64_t{6} * 60 * 60;
inline constexpr size_t kSkillBodyBytes = KiB(512);
inline constexpr size_t kSkillDescriptionBytes = 1024;
inline constexpr size_t kMaxSkills = 64;
inline constexpr size_t kProjectDocBytes = KiB(32);
inline constexpr size_t kMaxPendingAttachments = 8;
// MCP servers are untrusted; these bound what one may make the agent hold.
inline constexpr int64_t kMaxMcpServers = 32;
// Startup window the optional servers share before discovery moves to a turn
// boundary.
inline constexpr int64_t kMcpStartupGraceSeconds = 2;
inline constexpr int64_t kMcpPages = 100;
inline constexpr size_t kMaxMcpTools = 256;
inline constexpr size_t kMcpConfigBytes = MiB(1);
inline constexpr size_t kMcpResponseBytes = MiB(16);
inline constexpr size_t kMcpSchemaBytes = KiB(256);
inline constexpr size_t kMcpLogBytes = MiB(16);
inline constexpr size_t kMcpDescriptionChars = 400;
// Retention of pruned artifact trees: days kept, then newest-N kept.
inline constexpr int64_t kHistoryFiles = 200;
inline constexpr int64_t kDebugDays = 14;
inline constexpr int64_t kDebugFiles = 50;
inline constexpr int64_t kBgDays = 7;
inline constexpr int64_t kBgFiles = 200;
inline constexpr int64_t kMcpLogDays = 7;
inline constexpr int64_t kMcpLogFiles = 100;
inline constexpr int64_t kTerminalDays = 7;
// Minutes with nothing to do before a session's runtime or Chrome stops; the
// next message, mail or browser action starts it again.
inline constexpr int64_t kIdleMinutes = 15;

// Host and runtime pacing. The web host rescans the session catalogue at
// most once a second and keeps a few saved histories parsed for paging; a
// long text is read a page at a time; a stopping runtime gives its clients
// this long to take what is queued for them. An hour of silence in a panel
// usually means a new topic.
inline constexpr int64_t kCatalogueRescanSeconds = 1;
inline constexpr size_t kHistoryCacheEntries = 3;
inline constexpr size_t kHistoryCacheBytes = MiB(32);
inline constexpr size_t kTextPageBytes = KiB(16);
inline constexpr int64_t kServerDrainMs = 500;
inline constexpr int64_t kSecondsPerHour = 3600;
// Heads of files: what is read to tell a file's kind (an SVG prolog may run
// this long), the text of an attached text file the model reads inline, and
// the newest output an `until` marker is looked for in.
inline constexpr size_t kAttachmentSniffBytes = KiB(64);
inline constexpr size_t kAttachedTextBytes = KiB(64);
inline constexpr size_t kUntilWindowBytes = KiB(64);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_LIMITS_H_
