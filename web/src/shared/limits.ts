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
