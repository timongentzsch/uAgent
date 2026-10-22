import { stored, tabStorage } from "../shared/storage.ts";
// Where each transcript was left: following its end, or a message (and the
// paragraph inside it) with its offset. Kept in browser storage for this tab.
import { maxTranscriptBookmarks } from "../shared/limits.ts";
import { storageKeys } from "../shared/storage-keys.ts";

export type Bookmark = {
  follow: boolean;
  message?: string;
  inner?: string;
  offset?: number;
};

// Bookmarks change on every scroll frame; storage only needs the last one
// before the page goes away, so writes wait for scrolling to pause.
const kept = stored(
  storageKeys.bookmarks,
  new Map<string, Bookmark>(),
  (value) =>
    new Map(
      Object.entries(value && typeof value === "object" ? value : {})
        .filter(([, bookmark]) => typeof bookmark?.follow === "boolean")
        .slice(-maxTranscriptBookmarks),
    ),
  { where: tabStorage, delay: 500, kept: Object.fromEntries },
);
export const bookmarks = kept.read();

export function storeBookmark(key: string, value: Bookmark) {
  bookmarks.delete(key);
  bookmarks.set(key, value);
  while (bookmarks.size > maxTranscriptBookmarks)
    bookmarks.delete(bookmarks.keys().next().value!);
  kept.write(bookmarks);
}
