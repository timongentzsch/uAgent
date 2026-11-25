// Each key is named once, here. The values never change, or what is already
// kept would be lost; public/theme.js, which runs before any module, repeats
// theme, zoom and panes.
export const storageKeys = {
  catalogue: "uagent-catalogue",
  spaces: "uagent-spaces",
  pins: "uagent-pins",
  // The host's detail level and thinking, as last listed.
  display: "uagent-display",
  // The remote hosts last listed, shown disconnected until the host answers.
  hosts: "uagent-hosts",
  page: "uagent-page",
  draftPrefix: "uagent-drafts:",
  outgoing: "uagent-outgoing",
  read: "uagent-read",
  unread: "uagent-unread",
  browser: "uagent-browser",
  // The side chats each conversation shows as tabs on this device.
  sideTabs: "uagent-side-tabs",
  bookmarks: "uagent-transcript-bookmarks",
  theme: "uagent-theme",
  motion: "uagent-motion",
  zoom: "uagent-zoom",
  time: "uagent-time",
  panes: "uagent-panes",
  detail: "uagent-detail",
  thinking: "uagent-thinking",
  toggles: "uagent-toggles",
  // The sidebar's view: Recent or Places.
  list: "uagent-list",
  scheduledDraft: "uagent-scheduled-draft",
  overview: "uagent-overview-layout",
  // The folder Files last showed, by the conversation's folder.
  filesAt: "uagent-files-at",
  library: (kind: string, cwd: string) => `uagent-library-${kind}-${cwd}`,
  // Places of the sidebar's list remembered by path, e.g. folded ones.
  places: (name: string) => `uagent.sidebar.${name}`,
};

// Local keeps its existing keys; remote content is qualified by its owner.
export const hostStorageKey = (key: string, host: string) =>
  host ? `${key}:host:${host}` : key;
