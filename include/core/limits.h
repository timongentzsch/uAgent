// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_LIMITS_H_
#define UAGENT_INCLUDE_CORE_LIMITS_H_
// Compile-time policy constants; user-facing settings belong in
// config_registry.h. Keep this header dependency free: it is included widely.

#include <cstddef>
#include <cstdint>

namespace uagent {

inline constexpr size_t kKiB = 1024;
inline constexpr size_t kMiB = size_t{1024} * 1024;

inline constexpr size_t KiB(size_t n) { return n * kKiB; }
inline constexpr size_t MiB(size_t n) { return n * kMiB; }

// Separate budgets although equal: growing one must not grow the other.
inline constexpr size_t kPreviewChars = 4096;
inline constexpr size_t kCatalogueHeaderBytes = 4096;
// The opening of a diff that a tool result carries; the rest is an artifact.
inline constexpr size_t kChangePreviewChars = KiB(16);
// Cap of the encoded attachments kept for the model calls of a turn.
inline constexpr size_t kAttachmentCacheBytes = MiB(16);
// Entries held in a session or asset catalogue scan.
inline constexpr size_t kMaxCatalogueEntries = 4096;
// Pinned conversations and folders, and the bytes the list may take on disk.
inline constexpr size_t kMaxPins = 64;
inline constexpr size_t kPinsFileBytes = KiB(64);

inline constexpr size_t kMaxSkillFiles = 128;
inline constexpr int kMaxSkillFileDepth = 4;
inline constexpr size_t kLibraryNameChars = 100;
inline constexpr size_t kMaxSessionAssets = 64;
inline constexpr size_t kMaxToolsPerMessage = 32;
inline constexpr size_t kMaxActiveExchanges = 32;
inline constexpr size_t kMaxPendingSessionCommands = 32;
inline constexpr size_t kMaxApprovedProposals = 16;
// Overrides may name tools of an absent MCP registry, so both are bounded.
inline constexpr size_t kMaxToolSelectionOverrides = 256;
inline constexpr size_t kToolNameChars = 128;

inline constexpr size_t kToolDedupeMinChars = 256;
inline constexpr size_t kGenericTitleReplacementMinChars = 12;
// Generated titles: the first-message excerpt sent, and the title kept.
inline constexpr size_t kTitleInputChars = 2000;
inline constexpr size_t kTitleChars = 80;
// The terminal lines a thought takes where its work is open but it is not.
inline constexpr size_t kThoughtLines = 2;
// A remote host keeps a build no main has connected with for this long.
inline constexpr int kRemoteBuildDays = 30;
// The longest title a user may give a session, and the longest workspace
// name, in characters. The web form mirrors both (web/src/shared/limits.ts).
inline constexpr size_t kUserTitleChars = 256;
inline constexpr size_t kSpaceNameChars = 64;
inline constexpr int64_t kTitleTimeoutSeconds = 30;
// Enough for a title once the model does not think first.
inline constexpr int64_t kTitleMaxTokens = 96;
// Upload and subsequent claim validation must accept the same display names.
inline constexpr size_t kAssetNameChars = 128;
inline constexpr size_t kImageAnalysisCacheEntries = 8;
inline constexpr size_t kModelPickerMatches = 256;
inline constexpr size_t kConfigurationChangeLimit = 64;
inline constexpr size_t kSharedToolResultChars = 2000;
inline constexpr size_t kRetainedArtifactPathChars = 4096;
inline constexpr size_t kKqueueWatchTargets = 64;
inline constexpr int64_t kModelRequestDeadlineReserveSeconds = 1;
// What the answering round of a turn stopped at a limit may take.
inline constexpr int64_t kLimitAnswerSeconds = 60;

// Shared by model callbacks and browser publication.
inline constexpr int64_t kUsageProgressIntervalMs = 100;
// Progress is also reported after this many bytes, inside the interval.
inline constexpr size_t kUsageProgressBytes = KiB(1);
inline constexpr int64_t kStreamBatchIntervalMs = 12;
inline constexpr size_t kStreamBatchBytes = KiB(8);
// Short activity captions are presentation policies, not model limits.
inline constexpr size_t kStatusLabelBytes = 160;
// A background command's full command, kept with its row and its result.
inline constexpr size_t kActivityCommandBytes = KiB(8);
inline constexpr size_t kStatusLineBytes = 192;

// A repeated call is advised, then directed, then stopped. A rejected call
// stops sooner: rerunning it cannot produce new evidence.
inline constexpr int64_t kRepeatedCallAdviseAfter = 3;
inline constexpr int64_t kRepeatedCallDirectAfter = 6;
inline constexpr int64_t kRejectedCallStopAfter = 3;
inline constexpr int64_t kFailedToolAdviseAfter = 3;
inline constexpr int64_t kActivityPollAdviseAfter = 2;
inline constexpr int64_t kActivityPollDirectAfter = 4;
inline constexpr int64_t kActivityPollStopAfter = 12;

// A year is 365 days, as in HTTP `max-age`.
inline constexpr int64_t kSecondsPerMinute = 60;
inline constexpr int64_t kSecondsPerYear = 31536000;
inline constexpr int64_t kMillisecondsPerSecond = 1000;
// Latest epoch accepted by schedule validation (2100-01-01T00:00:00Z).
inline constexpr int64_t kMaxScheduleEpoch = 4102444800LL;

// A route under kCompactionSmallWindowTokens reads twice its window in bytes,
// never below the floor; carried-over skills and user messages are capped.
inline constexpr size_t kCompactionTranscriptBytes = KiB(256);
inline constexpr size_t kCompactionTranscriptFloorBytes = KiB(16);
inline constexpr int64_t kCompactionSmallWindowTokens = int64_t{128} * 1024;
inline constexpr size_t kCompactionSkillBytes = KiB(32);
inline constexpr size_t kCompactionUserBytes = KiB(80);
inline constexpr size_t kCompactionRouteFloorBytes = KiB(4);

// A poll tick re-checks something that has no event to wake on; a wait tick
// bounds a blocking wait so an abort or a spinner is noticed.
inline constexpr int kPollTickMs = 20;
inline constexpr int kWaitTickMs = 100;

// A proposal waits this long for the user's decision; an unclaimed upload or
// import is an orphan after a day; a host with no wake due sleeps a day.
inline constexpr int64_t kProposalLifetimeMinutes = 5;
inline constexpr int64_t kOrphanAgeHours = 24;
inline constexpr int64_t kIdleSleepHours = 24;

// Short digest widths; request construction and view projection share them.
inline constexpr size_t kDigestChars = 12;
inline constexpr size_t kOccurrenceChars = 16;
inline constexpr size_t kDetailChars = 24;
inline constexpr size_t kAgentNameChars = 8;

// The assembled system message, and what an external editor may return.
inline constexpr size_t kSystemPromptBytes = KiB(64);
// The journal's size before compaction, and the longest event line taken.
inline constexpr size_t kMemoryEventJournalBytes = KiB(256);
inline constexpr size_t kMemoryEventLineBytes = 4096;

// Owner-only: a shared machine cannot read transcripts or credentials.
inline constexpr int kPrivateFileMode = 0600;
inline constexpr int kPrivateDirMode = 0700;
// The group and other permission bits; a private file has none set.
inline constexpr int kGroupOtherModeBits = 0077;

// Files written into the user's folder follow the world-readable convention.
inline constexpr int kSharedFileMode = 0644;

// Bounds on tool arguments; the schemas the model sees are built from these.
inline constexpr int64_t kToolTextArgChars = 65536;
inline constexpr int64_t kToolMinOutputChars = 256;
inline constexpr int64_t kToolArgChars = 4096;
inline constexpr int64_t kPtyMaxDimension = 1000;

// Bounds for the `run` tool's initial yield: the lower one keeps a yield from
// becoming a busy poll, the upper one keeps a call from consuming the turn.
inline constexpr int64_t kMinYieldMs = 250;
inline constexpr int64_t kMaxYieldMs = 30000;
inline constexpr int64_t kDefaultYieldMs = 10000;

// Settle time for an activity write or resize that named no wait_ms.
inline constexpr int64_t kActivityInputSettleMs = 250;

inline constexpr size_t kRequestBytes = MiB(64);
inline constexpr size_t kResponseBytes = MiB(32);
inline constexpr size_t kToolTraceProtectChars = KiB(64);
inline constexpr size_t kToolTracePruneMinChars = KiB(32);
inline constexpr int64_t kSessionArchiveBytes = MiB(16);
inline constexpr int64_t kReadFileMaxLines = 10000;
inline constexpr int64_t kListDirScanEntries = 100000;
// The web file browser: bytes of a file shown as text, paths a search answers.
inline constexpr size_t kFileViewBytes = MiB(1);
inline constexpr size_t kFileFindMatches = 200;
// The web terminal: shells at once; twice as many connections, so a returning
// device attaches before its dropped one times out; scrollback; message size.
inline constexpr size_t kWebTerminals = 8;
inline constexpr size_t kWebTerminalSockets = 16;
inline constexpr size_t kWebTerminalScrollbackBytes = KiB(256);
inline constexpr size_t kWebTerminalMessageBytes = KiB(64);
// A hosted search's own work counts against this as well as its answer.
inline constexpr int64_t kWebSearchMaxTokens = 6000;
inline constexpr size_t kMemoryExtractBytes = KiB(32);
inline constexpr int64_t kMemoryExtractSeconds = 300;
// A scope this full of its limit, as a fraction, lets an extraction fold one
// of its notes into the one it writes.
inline constexpr size_t kMemoryCrowdedParts = 3, kMemoryCrowdedOf = 4;
// What placing an unfiled conversation shows the model: this many of the
// newest conversations, and this much of the latest reply.
inline constexpr size_t kPlacementCandidates = 30;
inline constexpr size_t kPlacementReplyChars = 2000;
inline constexpr size_t kSkillBodyBytes = KiB(512);
// The most of a memory file the library reads.
inline constexpr size_t kMemoryFileBytes = KiB(512);
inline constexpr size_t kSkillDescriptionBytes = 1024;
// MCP servers are untrusted; these bound what one may make the agent hold.
inline constexpr int64_t kMcpStartupGraceSeconds = 2;
inline constexpr int64_t kMcpPages = 100;
inline constexpr size_t kMcpConfigBytes = MiB(1);
inline constexpr size_t kMcpResponseBytes = MiB(16);
// The Linux sandbox trampoline receives its policy as one argv word, which the
// kernel caps at 128 KiB.
inline constexpr size_t kSandboxPolicyArgBytes = KiB(96);
inline constexpr size_t kMcpSchemaBytes = KiB(256);
inline constexpr size_t kMcpLogBytes = MiB(16);
inline constexpr size_t kMcpDescriptionChars = 400;
// Stamp file and interval of the pass over every artifact directory.
inline constexpr char kMaintenanceStamp[] = ".maintained";
inline constexpr int64_t kMaintenanceIntervalHours = 24;
// How often a stoppable log pump checks whether it was asked to stop.
inline constexpr int kLogPumpPollMs = 100;

// The web host rescans the session catalogue this often and keeps a few
// histories parsed; a stopping runtime gives its clients kServerDrainMs.
inline constexpr int64_t kCatalogueRescanSeconds = 1;
inline constexpr size_t kHistoryCacheEntries = 3;
inline constexpr size_t kHistoryCacheBytes = MiB(32);
inline constexpr size_t kTextPageBytes = KiB(16);
inline constexpr int64_t kServerDrainMs = 500;
inline constexpr int64_t kSecondsPerHour = 3600;
// Heads read: to tell a file's kind (an SVG prolog may run this long), of an
// attached text file, and of the newest output an `until` marker is sought in.
inline constexpr size_t kAttachmentSniffBytes = KiB(64);
inline constexpr size_t kAttachedTextBytes = KiB(64);
inline constexpr size_t kUntilWindowBytes = KiB(64);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_LIMITS_H_
