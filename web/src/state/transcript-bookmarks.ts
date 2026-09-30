// Where each transcript was left: following its end, or a message (and the
// paragraph inside it) with its offset. Kept per tab in sessionStorage.
import { maxTranscriptBookmarks } from "../shared/limits.ts";

export type Bookmark = {
  follow: boolean;
  message?: string;
  inner?: string;
  offset?: number;
};

const BOOKMARKS_KEY = "uagent-transcript-bookmarks";
const SAVE_MS = 500;
export const bookmarks = new Map<string, Bookmark>();
try {
  for (const [key, value] of Object.entries(
    JSON.parse(sessionStorage.getItem(BOOKMARKS_KEY) || "{}"),
  ).slice(-maxTranscriptBookmarks)) {
    if (typeof (value as Bookmark)?.follow === "boolean")
      bookmarks.set(key, value as Bookmark);
  }
} catch {
  // Private browsing can deny session storage. In-memory bookmarks still work.
}

// Bookmarks change on every scroll frame; storage only needs the last one
// before the page goes away, so writes trail by SAVE_MS and flush on hide.
let saving: ReturnType<typeof setTimeout> | undefined;
function saveBookmarks() {
  clearTimeout(saving);
  saving = undefined;
  try {
    sessionStorage.setItem(
      BOOKMARKS_KEY,
      JSON.stringify(Object.fromEntries(bookmarks)),
    );
  } catch {
    // In-memory restoration still works when storage is unavailable.
  }
}
addEventListener("pagehide", () => saving && saveBookmarks());

export function storeBookmark(key: string, value: Bookmark) {
  bookmarks.delete(key);
  bookmarks.set(key, value);
  while (bookmarks.size > maxTranscriptBookmarks)
    bookmarks.delete(bookmarks.keys().next().value!);
  saving ??= setTimeout(saveBookmarks, SAVE_MS);
}
