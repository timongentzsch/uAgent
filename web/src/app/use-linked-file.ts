import { useEffect, useRef } from "preact/hooks";
import type { View } from "../features/files/flow.ts";
import {
  fileFromURL,
  fileOriginFromURL,
  replaceLinkedFile,
  selectedFromURL,
} from "../shared/navigation.ts";

// Files normally keeps its navigation on this device. When opened from
// a link, its current file also follows the URL through reload and history.
export function useLinkedFile(
  session: string,
  open: boolean,
  view: View | undefined,
  show: (path: string, origin?: string) => void,
) {
  const linked = useRef<
    | { session: string; path: string; origin?: string; opened: boolean }
    | undefined
  >(undefined);
  useEffect(() => {
    const follow = () => {
      if (selectedFromURL() !== session) return;
      const path = fileFromURL();
      const origin = fileOriginFromURL();
      if (!path) {
        if (linked.current?.session === session) show("");
        linked.current = undefined;
        return;
      }
      if (
        !session ||
        (linked.current?.session === session &&
          linked.current.path === path &&
          linked.current.origin === origin)
      )
        return;
      linked.current = { session, path, origin, opened: false };
      show(path, origin);
    };
    follow();
    addEventListener("hashchange", follow);
    return () => removeEventListener("hashchange", follow);
  }, [session, show]);

  const wasOpen = useRef(false);
  useEffect(() => {
    const link = linked.current;
    const closed = wasOpen.current && !open;
    wasOpen.current = open;
    if (!link || link.session !== session) return;
    if (closed) {
      replaceLinkedFile(session, link.path);
      linked.current = undefined;
      return;
    }
    if (!open || !view) return;
    if (view.kind === "list" && view.note) link.opened = true;
    if (view.kind !== "file" && view.kind !== "sequence") return;
    // The previous file can still be on screen as a new address arrives.
    if (view.path === link.path) link.opened = true;
    else if (link.opened && replaceLinkedFile(session, link.path, view.path))
      link.path = view.path;
  }, [session, open, view]);
}
