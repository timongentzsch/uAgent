import { useCallback, useLayoutEffect, useRef, useState } from "preact/hooks";
import { maxTranscriptBookmarks } from "../shared/limits.ts";

type Bookmark = {
  follow: boolean;
  message?: string;
  inner?: string;
  offset?: number;
};
type Anchor = {
  node: HTMLElement;
  message: string;
  inner?: string;
  contentY: number;
  height: number;
  within: number;
};

const BOOKMARKS_KEY = "uagent-transcript-bookmarks";
const BOTTOM_BAND = 2;
const MOVE = 1;
const bookmarks = new Map<string, Bookmark>();
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

function storeBookmark(key: string, value: Bookmark) {
  bookmarks.delete(key);
  bookmarks.set(key, value);
  while (bookmarks.size > maxTranscriptBookmarks)
    bookmarks.delete(bookmarks.keys().next().value!);
  try {
    sessionStorage.setItem(
      BOOKMARKS_KEY,
      JSON.stringify(Object.fromEntries(bookmarks)),
    );
  } catch {
    // In-memory restoration still works when storage is unavailable.
  }
}

function contentY(node: HTMLElement, box: HTMLElement) {
  return (
    node.getBoundingClientRect().top -
    box.getBoundingClientRect().top -
    box.clientTop +
    box.scrollTop
  );
}

function offset(node: HTMLElement, box: HTMLElement) {
  return contentY(node, box) - box.scrollTop;
}

function rowFor(node: HTMLElement): HTMLElement | null {
  return node.closest<HTMLElement>("[data-message-id]");
}

function visibleAnchor(box: HTMLElement, column: HTMLElement): Anchor | null {
  const top = box.getBoundingClientRect().top + box.clientTop;
  const bottom = top + box.clientHeight;
  for (const child of column.children) {
    if (!(child instanceof HTMLElement) || !child.dataset.messageId) continue;
    const rect = child.getBoundingClientRect();
    if (rect.bottom <= top + 1 || rect.top >= bottom) continue;
    let node = child;
    for (const inner of child.querySelectorAll<HTMLElement>(
      "[data-anchor-id]",
    )) {
      const area = inner.getBoundingClientRect();
      if (area.bottom > top + 1 && area.top < bottom) {
        node = inner;
        break;
      }
    }
    const nodeRect = node.getBoundingClientRect();
    return {
      node,
      message: child.dataset.messageId,
      inner: node === child ? undefined : node.dataset.anchorId,
      contentY: contentY(node, box),
      height: nodeRect.height,
      within: Math.max(0, top - nodeRect.top),
    };
  }
  return null;
}

function findAnchor(
  column: HTMLElement,
  bookmark: Pick<Bookmark, "message" | "inner">,
): HTMLElement | null {
  for (const row of column.querySelectorAll<HTMLElement>("[data-message-id]")) {
    if (row.dataset.messageId !== bookmark.message) continue;
    if (bookmark.inner) {
      for (const inner of row.querySelectorAll<HTMLElement>(
        "[data-anchor-id]",
      )) {
        if (inner.dataset.anchorId === bookmark.inner) return inner;
      }
    }
    return row;
  }
  return null;
}

// One scroll owner for main and child transcripts. Both surfaces supply their
// rows and pagination; this hook owns follow intent, semantic restoration and
// compensation. Native scroll anchoring stays disabled on these scrollers.
export function useTranscriptHistory(
  onFollow: (following: boolean) => void,
  sessionKey: string,
  arrivals: number,
) {
  const scroller = useRef<HTMLDivElement>(null);
  const content = useRef<HTMLDivElement>(null);
  const [box, setBox] = useState<HTMLDivElement | null>(null);
  const [column, setColumn] = useState<HTMLDivElement | null>(null);
  const following = useRef(true);
  const anchor = useRef<Anchor | null>(null);
  const key = useRef(sessionKey);
  const pending = useRef<string | null>(sessionKey);
  const lastTop = useRef(0);
  const lastArrivals = useRef(arrivals);
  const [unseen, setUnseen] = useState(0);
  const onFollowRef = useRef(onFollow);
  onFollowRef.current = onFollow;
  const observer = useRef<ResizeObserver | null>(null);

  const attachScroller = useCallback((node: HTMLDivElement | null) => {
    scroller.current = node;
    setBox(node);
  }, []);
  const attachContent = useCallback((node: HTMLDivElement | null) => {
    content.current = node;
    setColumn(node);
  }, []);

  const writeTop = useCallback((top: number) => {
    const element = scroller.current;
    if (!element) return;
    const maximum = Math.max(0, element.scrollHeight - element.clientHeight);
    const target = Math.min(maximum, Math.max(0, top));
    if (Math.abs(element.scrollTop - target) > 0.5) {
      element.scrollTop = target;
    }
    lastTop.current = element.scrollTop;
  }, []);

  const capture = useCallback(() => {
    const element = scroller.current;
    if (!element) return;
    if (following.current) {
      storeBookmark(key.current, { follow: true });
      return;
    }
    const current = anchor.current;
    if (!current?.node.isConnected) return;
    storeBookmark(key.current, {
      follow: false,
      message: current.message,
      inner: current.inner,
      offset: offset(current.node, element),
    });
  }, []);

  const selectAnchor = useCallback(() => {
    const element = scroller.current;
    const children = content.current;
    if (!element || !children) return;
    anchor.current = visibleAnchor(element, children);
    capture();
  }, [capture]);

  const setFollow = useCallback((value: boolean) => {
    if (following.current === value) return;
    following.current = value;
    if (value) {
      anchor.current = null;
      setUnseen(0);
    }
    onFollowRef.current(value);
  }, []);

  const stopFollowing = useCallback(() => {
    if (!following.current) return;
    setFollow(false);
    selectAnchor();
  }, [selectAnchor, setFollow]);

  const pin = useCallback(() => {
    const element = scroller.current;
    if (element) writeTop(element.scrollHeight - element.clientHeight);
  }, [writeTop]);

  const reconcile = useCallback(() => {
    const element = scroller.current;
    if (!element) return;
    if (element.dataset.historyKey !== key.current) return;
    if (element.hasAttribute("aria-busy")) return;
    if (following.current) {
      pin();
      return;
    }
    const current = anchor.current;
    if (!current) {
      selectAnchor();
      return;
    }
    let replaced = false;
    if (!current.node.isConnected) {
      const replacement =
        content.current && findAnchor(content.current, current);
      if (!replacement) {
        selectAnchor();
        return;
      }
      current.node = replacement;
      replaced = true;
    }
    const next = contentY(current.node, element);
    const height = current.node.getBoundingClientRect().height;
    const within =
      replaced && current.height > 0
        ? current.within * (height / current.height)
        : current.within;
    const shift = next + within - current.contentY - current.within;
    if (Math.abs(shift) > 0.5) writeTop(element.scrollTop + shift);
    current.contentY = next;
    current.height = height;
    current.within = within;
    capture();
  }, [capture, pin, selectAnchor, writeTop]);

  const observe = useCallback(() => {
    const watch = observer.current;
    const element = scroller.current;
    const children = content.current;
    if (!watch || !element || !children) return;
    watch.disconnect();
    watch.observe(element);
    watch.observe(children);
    for (const child of children.children) {
      if (child instanceof HTMLElement) watch.observe(child);
    }
    const row = anchor.current && rowFor(anchor.current.node);
    if (row) {
      for (const inner of row.querySelectorAll<HTMLElement>("[data-anchor-id]"))
        watch.observe(inner);
    }
  }, []);

  const restore = useCallback(() => {
    const element = scroller.current;
    const children = content.current;
    if (
      !element ||
      !children ||
      element.hasAttribute("aria-busy") ||
      element.dataset.historyKey !== key.current ||
      pending.current !== key.current
    )
      return false;
    const saved = bookmarks.get(key.current);
    if (!saved || saved.follow) {
      setFollow(true);
      pin();
    } else {
      const node = findAnchor(children, saved);
      setFollow(false);
      if (node)
        writeTop(
          element.scrollTop + offset(node, element) - (saved.offset || 0),
        );
      anchor.current = node
        ? {
            node,
            message: rowFor(node)?.dataset.messageId || saved.message || "",
            inner: node.dataset.anchorId,
            contentY: contentY(node, element),
            height: node.getBoundingClientRect().height,
            within: Math.max(0, -(saved.offset || 0)),
          }
        : visibleAnchor(element, children);
    }
    pending.current = null;
    lastTop.current = element.scrollTop;
    observe();
    return true;
  }, [observe, pin, setFollow, writeTop]);

  useLayoutEffect(() => {
    if (key.current === sessionKey) return;
    capture();
    key.current = sessionKey;
    pending.current = sessionKey;
    anchor.current = null;
    lastArrivals.current = arrivals;
    setUnseen(0);
  }, [sessionKey]);

  // React commits and late browser layout use the same reconciliation path.
  useLayoutEffect(() => {
    if (!box?.isConnected || box !== scroller.current || !column) return;
    if (!restore()) reconcile();
    if (arrivals > lastArrivals.current && !following.current)
      setUnseen((value) => value + arrivals - lastArrivals.current);
    lastArrivals.current = arrivals;
  });

  useLayoutEffect(() => {
    if (!box || !column || box !== scroller.current) return;
    const settle = () => {
      if (!restore()) reconcile();
    };
    const watch = new ResizeObserver(settle);
    const mutations = new MutationObserver(() => {
      settle();
      observe();
    });
    const readiness = new MutationObserver(settle);
    observer.current = watch;
    observe();
    mutations.observe(column, {
      subtree: true,
      childList: true,
      characterData: true,
      attributes: true,
      attributeFilter: ["style", "class", "src"],
    });
    readiness.observe(box, {
      attributes: true,
      attributeFilter: ["aria-busy"],
    });
    let touchY = 0;
    const wheel = (event: WheelEvent) => {
      if (event.deltaY < 0) stopFollowing();
    };
    const touchStart = (event: TouchEvent) => {
      touchY = event.touches[0]?.clientY || 0;
    };
    const touchMove = (event: TouchEvent) => {
      const next = event.touches[0]?.clientY || touchY;
      if (next > touchY + MOVE) stopFollowing();
      touchY = next;
    };
    const keyDown = (event: KeyboardEvent) => {
      if (
        event.target instanceof HTMLElement &&
        event.target.closest("input,textarea,[contenteditable]")
      )
        return;
      if (["ArrowUp", "PageUp", "Home"].includes(event.key)) stopFollowing();
    };
    const scroll = () => {
      const top = box.scrollTop;
      const maximum = Math.max(0, box.scrollHeight - box.clientHeight);
      if (following.current) {
        if (maximum - top > BOTTOM_BAND && top < lastTop.current - MOVE)
          stopFollowing();
      } else {
        if (top > lastTop.current + MOVE && maximum - top <= BOTTOM_BAND) {
          setFollow(true);
          pin();
        } else {
          selectAnchor();
          observe();
        }
      }
      lastTop.current = box.scrollTop;
    };
    box.addEventListener("wheel", wheel, { passive: true });
    box.addEventListener("touchstart", touchStart, { passive: true });
    box.addEventListener("touchmove", touchMove, { passive: true });
    box.addEventListener("keydown", keyDown);
    box.addEventListener("scroll", scroll, { passive: true });
    return () => {
      watch.disconnect();
      mutations.disconnect();
      readiness.disconnect();
      if (observer.current === watch) observer.current = null;
      box.removeEventListener("wheel", wheel);
      box.removeEventListener("touchstart", touchStart);
      box.removeEventListener("touchmove", touchMove);
      box.removeEventListener("keydown", keyDown);
      box.removeEventListener("scroll", scroll);
    };
  }, [
    box,
    column,
    capture,
    observe,
    pin,
    reconcile,
    restore,
    selectAnchor,
    setFollow,
    stopFollowing,
  ]);

  const jumpToLatest = useCallback(() => {
    pending.current = null;
    setFollow(true);
    pin();
    capture();
  }, [capture, pin, setFollow]);

  const preserveWhile = useCallback(
    async (load: () => Promise<void>) => {
      stopFollowing();
      capture();
      await load();
    },
    [capture, stopFollowing],
  );

  return {
    scroller,
    content,
    attachScroller,
    attachContent,
    stopFollowing,
    preserveWhile,
    jumpToLatest,
    unseen,
  };
}
