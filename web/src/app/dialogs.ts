// Lazy dialog/sidebar registry for the app shell. One reason to change:
// which chunk backs each modal/page. The shell keeps all state; this only
// maps names to dynamic imports so route-level code-splitting stays in one
// place and out of the state machine.
export const sidebarModule = () => import("../features/sidebar/sidebar.tsx");
export const menuModule = () =>
  import("../features/sidebar/sidebar.tsx").then((module) => ({
    default: module.ConversationMenu,
  }));
export const libraryModule = () => import("../features/library/library.tsx");
export const scheduledModule = () =>
  import("../features/scheduled/scheduled.tsx");
export const pairing = () => import("../features/settings/pairing.tsx");
export const composer = () => import("../features/composer/composer.tsx");
export const chat = () => import("../features/chat/chat.tsx");
export const rawDialog = () => import("../features/settings/raw.tsx");
export const conversationActions = () =>
  import("../features/chat/conversation-actions.tsx");
export const statisticsDialog = () =>
  import("../features/settings/statistics.tsx");
export const promptDialog = () => import("../features/settings/prompt.tsx");
export const settingsDialog = () => import("../features/settings/settings.tsx");
