// Fixed browser memory and rendering policies. Runtime configurable native
// limits live in the C++ config registry; these bounds protect local UI state.
export const maxLocalRequests = 256;
export const maxAttentionReceipts = 256;
export const maxHttpExchanges = 64;
export const maxRecalledPromptSessions = 32;
// Prompts Up and Down recall per conversation.
export const maxRecalledPrompts = 100;
// Rows a transcript keeps as older pages are read in.
export const maxHistoryBlocks = 256;
export const reconnectMaxDelayMs = 30_000;
// A reconnect shorter than this (a resumed app) shows no status change.
export const reconnectGraceMs = 1_000;
export const maxTranscriptBookmarks = 20;
export const maxDiagramSourceChars = 20_000;
export const maxDiagramEdges = 300;
export const maxDiagramSvgChars = 512_000;
export const maxDiagramCacheEntries = 24;
export const maxHighlightChars = 32 * 1024;
// A file opened in the files pane is highlighted whole up to this size.
export const maxFileHighlightChars = 256 * 1024;

// Cache/rendering policies, measured in JS characters rather than provider tokens.
export const maxPreparedMarkdownChars = 4_000_000;
// Up to this size a streaming reply's finished part is parsed as a whole
// (lists and references across blank lines stay intact); past it, only new
// blocks are parsed and appended, so cost stays flat as the reply grows.
export const wholeStreamingMarkdownChars = 16_000;
export const retainedBackgroundViews = 4;
export const commandReceiptWaitMs = 30_000;
// How long a notification's link waits for its decision to be drawn.
export const decisionFocusWaitMs = 15_000;
// How long a closing conversation is given to retire before delete retries.
export const deleteRetryDelayMs = 1_000;
// How long a copy button shows that it copied.
export const copiedFeedbackMs = 2_000;
// A new scheduled task's interval, and how far ahead a one-off is set.
export const defaultScheduleSeconds = 3_600;

// A scheduled task's interval is at most a year, as the host has it
// (kSecondsPerYear); the form waits this long after an edit to ask the host
// when the schedule next fires.
export const maxScheduleMinutes = 525_600;
export const schedulePreviewDebounceMs = 200;

// A session's title and a workspace's name are bounded as on the host
// (kUserTitleChars, kSpaceNameChars), in characters; maxLength counts UTF-16
// units, never fewer, so the form is never laxer than the host.
export const maxTitleChars = 256;
export const maxSpaceNameChars = 64;

// Zoom shown and stored when the device has not chosen one.
export const defaultZoom = 100;
// Drafts are saved once typing pauses this long, and when the page hides.
export const draftSaveDelayMs = 500;
// A second press of Stop within this is the first one bouncing, not a decision.
export const stopGuardMs = 500;
// The first reconnect delay; it doubles with each failure up to
// reconnectMaxDelayMs.
export const reconnectBaseDelayMs = 1_000;
// An object URL handed to a download outlives the click by this long, for
// browsers that start the download afterwards.
export const downloadRevokeMs = 30_000;
// A tool argument this short reads as a fact beside the tool's name.
export const maxInlineFactChars = 40;

// A draft's attachments: how many, and the largest upload the host takes
// (kUploadCount, kUploadBytes).
export const maxDraftFiles = 8;
export const maxUploadBytes = 8 * 1024 * 1024;

// Mobile Safari avoids focus zoom for an editable layout font of at least 16px.
export const focusFontFloor = 16;
