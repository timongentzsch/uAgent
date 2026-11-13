// Fixed browser memory and rendering policies. Runtime configurable native
// limits live in the C++ config registry; these bounds protect local UI state.

export function dropOldest<K>(
  entries: { size: number; keys(): Iterable<K>; delete(key: K): unknown },
  max: number,
) {
  for (const key of entries.keys()) {
    if (entries.size <= max) return;
    entries.delete(key);
  }
}

export const maxLocalRequests = 256;
export const maxAttentionReceipts = 256;
export const maxHttpExchanges = 64;
export const maxRecalledPromptSessions = 32;
// Prompts Up and Down recall per conversation.
export const maxRecalledPrompts = 100;
export const maxHistoryNodes = 256;
// A row with more children shows its latest ones and a line for the rest.
export const maxShownSteps = 30;
export const maxRememberedToggles = 500;
export const reconnectMaxDelayMs = 30_000;
export const hostsPollMs = 2_000;
export const hostLatencyPollMs = 5_000;
// A reconnect shorter than this (a resumed app) shows no status change.
export const reconnectGraceMs = 1_000;
export const maxTranscriptBookmarks = 20;
export const maxDiagramSourceChars = 20_000;
export const maxDiagramEdges = 300;
export const maxDiagramSvgChars = 512_000;
export const maxDiagramCacheEntries = 24;
export const maxHighlightChars = 32 * 1024;
// So a block that streamed in is not highlighted again when rendered whole.
export const maxHighlightCacheEntries = 64;
export const maxFileHighlightChars = 256 * 1024;

// Measured in JS characters, not provider tokens.
export const maxPreparedMarkdownChars = 1_000_000;
// Up to this size a streaming reply's finished part is parsed as a whole
// (lists and references across blank lines stay intact); past it, only new
// blocks are parsed and appended, so cost stays flat as the reply grows.
export const wholeStreamingMarkdownChars = 16_000;
export const retainedBackgroundViews = 4;
export const commandReceiptWaitMs = 30_000;
// How long a notification's link waits for its decision to be drawn.
export const decisionFocusWaitMs = 15_000;
export const deleteRetryDelayMs = 1_000;
export const copiedFeedbackMs = 2_000;
// A new scheduled task's interval, and how far ahead a one-off is set.
export const defaultScheduleSeconds = 3_600;

// At most a year, as the host has it (kSecondsPerYear).
export const maxScheduleMinutes = 525_600;
export const schedulePreviewDebounceMs = 200;

// A session's title and a workspace's name are bounded as on the host
// (kUserTitleChars, kSpaceNameChars), in characters; maxLength counts UTF-16
// units, never fewer, so the form is never laxer than the host.
export const maxTitleChars = 256;
export const maxSpaceNameChars = 64;
// How long a conversation nothing was suggested for stays under To file.
export const unfiledDays = 7;
// The workspace a conversation starts in when no folder is chosen.
export const scratchSpace = "Scratch";

export const defaultZoom = 100;
export const draftSaveDelayMs = 500;
export const catalogueSaveDelayMs = 1_000;
// A second press of Stop within this is the first one bouncing.
export const stopGuardMs = 500;
// Doubles with each failure up to reconnectMaxDelayMs.
export const reconnectBaseDelayMs = 1_000;
// An object URL handed to a download outlives the click by this long, for
// browsers that start the download afterwards.
export const downloadRevokeMs = 30_000;

// As the host has them (kUploadCount, kUploadBytes).
export const maxDraftFiles = 8;
export const maxUploadBytes = 8 * 1024 * 1024;
// The host writes no larger text file.
export const maxTextFileBytes = 1024 * 1024;

// Mobile Safari avoids focus zoom for an editable layout font of at least 16px.
export const focusFontFloor = 16;

// maxTerminals is the host's kWebTerminals; "Add to chat" carries the end
// of a terminal, at most maxTerminalChatChars.
export const maxTerminals = 8;
export const maxTerminalChatChars = 16_000;
export const terminalScrollbackLines = 5_000;
// A touch that holds still this long on a list row opens its menu; one that
// moves farther than the slop is a scroll.
export const longPressMs = 450;
export const longPressSlopPx = 10;
export const swipeSlopPx = 10;
// A sheet pulled down by this share of its height closes when let go, as
// does one pulled faster than the flick speed (pixels per millisecond).
export const sheetCloseShare = 0.3;
export const sheetFlickSpeed = 0.6;
export const edgeSwipeStartPx = 24;
export const edgeSwipeOpenPx = 60;
