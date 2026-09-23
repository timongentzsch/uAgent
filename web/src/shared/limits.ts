// Fixed browser memory and rendering policies. Runtime configurable native
// limits live in the C++ config registry; these bounds protect local UI state.
export const maxLocalRequests = 256;
export const maxAttentionReceipts = 256;
export const maxTranscriptBookmarks = 20;
export const maxLivePreviewChars = 64 * 1024;
export const maxDiagramSourceChars = 20_000;
export const maxDiagramEdges = 300;
export const maxDiagramSvgChars = 512_000;
export const maxDiagramCacheEntries = 24;
export const maxHighlightChars = 32 * 1024;

// Cache/rendering policies, measured in JS characters rather than provider tokens.
export const maxPreparedMarkdownChars = 4_000_000;
export const maxProgressiveMarkdownChars = 16_000;
export const progressiveMarkdownIntervalMs = 120;
export const progressiveMarkdownScanLines = 40;
export const retainedBackgroundViews = 4;
export const commandReceiptWaitMs = 30_000;
