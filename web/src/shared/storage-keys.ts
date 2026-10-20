// What this browser keeps, by the key it is kept under: each key is named
// once, here. The values never change, or what is already kept would be
// lost; public/theme.js, which runs before any module, repeats theme, zoom
// and panes.
export const storageKeys = {
  catalogue: "uagent-catalogue",
  drafts: "uagent-drafts",
  outgoing: "uagent-outgoing",
  read: "uagent-read",
  unread: "uagent-unread",
  browser: "uagent-browser",
  sideConversations: "uagent-side-conversations",
  bookmarks: "uagent-transcript-bookmarks",
  theme: "uagent-theme",
  motion: "uagent-motion",
  zoom: "uagent-zoom",
  time: "uagent-time",
  panes: "uagent-panes",
  scheduledDraft: "uagent-scheduled-draft",
  library: (kind: string, cwd: string) => `uagent-library-${kind}-${cwd}`,
  // Places of the sidebar's list remembered by path, e.g. folded ones.
  places: (name: string) => `uagent.sidebar.${name}`,
};
