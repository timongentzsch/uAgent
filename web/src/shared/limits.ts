// Fixed browser memory and rendering policies. Runtime configurable native
// limits live in the C++ config registry; these bounds protect local UI state.
export const maxLocalRequests = 256;
export const maxAttentionReceipts = 256;
export const maxHttpExchanges = 64;
export const maxRecalledPromptSessions = 32;
export const reconnectMaxDelayMs = 30_000;
// A reconnect shorter than this (a resumed app) shows no status change.
export const reconnectGraceMs = 1_000;
export const maxTranscriptBookmarks = 20;
export const maxLivePreviewChars = 64 * 1024;
export const maxDiagramSourceChars = 20_000;
export const maxDiagramEdges = 300;
export const maxDiagramSvgChars = 512_000;
export const maxDiagramCacheEntries = 24;
export const maxHighlightChars = 32 * 1024;

// Cache/rendering policies, measured in JS characters rather than provider tokens.
export const maxPreparedMarkdownChars = 4_000_000;
// Up to this size a streaming reply's finished part is parsed as a whole
// (lists and references across blank lines stay intact); past it, only new
// blocks are parsed and appended, so cost stays flat as the reply grows.
export const wholeStreamingMarkdownChars = 16_000;
export const progressiveMarkdownScanLines = 40;
export const retainedBackgroundViews = 4;
export const commandReceiptWaitMs = 30_000;

// Mobile Safari avoids focus zoom for an editable layout font of at least 16px.
export const focusFontFloor = 16;
