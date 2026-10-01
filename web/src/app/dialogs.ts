// Lazy dialog/page registry for the app shell. One reason to change:
// which chunk backs each modal/page. The shell keeps all state; this only
// maps names to dynamic imports so route-level code-splitting stays in one
// place and out of the state machine.
export const libraryModule = () => import("../features/library/library.tsx");
export const scheduledModule = () =>
  import("../features/scheduled/scheduled.tsx");
export const pairing = () => import("../features/pairing/pairing.tsx");
export const browserDialog = () => import("../features/browser/browser.tsx");
export const rawDialog = () => import("../features/chat/raw.tsx");
export const inspectorDialog = () => import("../features/chat/inspector.tsx");
export const sideAnswer = () => import("../features/chat/side-answer.tsx");
export const conversationActions = () =>
  import("../features/chat/conversation-actions.tsx");
export const statisticsDialog = () => import("../features/chat/statistics.tsx");
export const instructionsDialog = () =>
  import("../features/settings/instructions.tsx");
export const settingsDialog = () => import("../features/settings/settings.tsx");
export const toolsDialog = () => import("../features/settings/tools.tsx");
export const paletteDialog = () => import("../features/palette/palette.tsx");
export const shortcutsDialog = () =>
  paletteDialog().then((module) => ({ default: module.ShortcutList }));
