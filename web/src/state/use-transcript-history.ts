import { useCallback, useLayoutEffect, useRef, useState } from "preact/hooks";
import {
  bookmarks,
  storeBookmark,
  type Bookmark,
} from "./transcript-bookmarks.ts";

type Anchor = {
  node: HTMLElement;
  message: string;
  inner?: string;
  contentY: number;
  height: number;
  within: number;
};

const BOTTOM_BAND = 2;
const MOVE = 1;
// A wheel event's scroll lands after it, over one scroll event or an animated
// run of them (Linux WebKit). WebKit cancels that scroll when scrollTop is
// written meanwhile, so compensation waits for the wheel's first scroll and
// then for the scrolling to go quiet.
const WHEEL_SETTLE_MS = 250;
const WHEEL_QUIET_MS = 80;
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

function atEnd(box: HTMLElement) {
  return box.scrollHeight - box.clientHeight - box.scrollTop <= BOTTOM_BAND;
}

// Whether an upward gesture from target reaches the transcript itself: a
// nested scroller (tool output) takes it first while it can still move up.
function scrollsUp(target: EventTarget | null, box: HTMLElement) {
  for (
    let node = target instanceof Element ? target : null;
    node && node !== box;
    node = node.parentElement
  ) {
    if (node.scrollTop > 0 && node.scrollHeight > node.clientHeight)
      return false;
  }
  return box.scrollTop > 0;
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
  blocks: readonly { id: string }[],
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
  // The newest block seen: unseen counts only blocks appended after it, so
  // an older history page prepended above never reads as new.
  const lastBlock = useRef(blocks.at(-1)?.id);
  const [unseen, setUnseen] = useState(0);
  const onFollowRef = useRef(onFollow);
  onFollowRef.current = onFollow;
  const observer = useRef<ResizeObserver | null>(null);
  // What the observer watches, so a refresh observes only new nodes: each
  // observe() makes the observer report that node once more.
  const watched = useRef(new Set<Element>());
  const wheelAt = useRef(-Infinity);
  // A compensation waiting for the wheel's scroll to land.
  const deferred = useRef<ReturnType<typeof setTimeout> | null>(null);

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

  // A reader who moves down into the end follows again: by scrolling there,
  // or by a wheel, swipe or key that finds no more room below. Only the
  // reader's own downward intent counts, so a shrink that clamps the range
  // (see reconcile) never re-follows on its own.
  const resumeAtEnd = useCallback(() => {
    const element = scroller.current;
    if (following.current || !element || !atEnd(element)) return false;
    if (deferred.current !== null) {
      clearTimeout(deferred.current);
      deferred.current = null;
    }
    setFollow(true);
    pin();
    return true;
  }, [pin, setFollow]);

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
    const waited = performance.now() - wheelAt.current;
    if (waited < WHEEL_SETTLE_MS) {
      deferred.current ??= setTimeout(settleWheel, WHEEL_SETTLE_MS - waited);
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

  // Applies a waiting compensation once the wheel's scroll has gone quiet,
  // or never started.
  const settleWheel = () => {
    if (deferred.current === null) return;
    clearTimeout(deferred.current);
    deferred.current = null;
    wheelAt.current = -Infinity;
    reconcileRef.current();
  };
  const reconcileRef = useRef(reconcile);
  reconcileRef.current = reconcile;

  const observe = useCallback(() => {
    const watch = observer.current;
    const element = scroller.current;
    const children = content.current;
    if (!watch || !element || !children) return;
    const next = new Set<Element>([element, children, ...children.children]);
    const row = anchor.current && rowFor(anchor.current.node);
    if (row) {
      for (const inner of row.querySelectorAll("[data-anchor-id]"))
        next.add(inner);
    }
    for (const node of watched.current)
      if (!next.has(node)) watch.unobserve(node);
    for (const node of next)
      if (!watched.current.has(node)) watch.observe(node);
    watched.current = next;
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
    lastBlock.current = blocks.at(-1)?.id;
    setUnseen(0);
  }, [sessionKey]);

  // React commits and late browser layout use the same reconciliation path.
  useLayoutEffect(() => {
    if (!box?.isConnected || box !== scroller.current || !column) return;
    if (!restore()) reconcile();
    const last = blocks.at(-1)?.id;
    if (last !== lastBlock.current) {
      if (!following.current) {
        const seen = blocks.findLastIndex(
          (block) => block.id === lastBlock.current,
        );
        if (seen >= 0) setUnseen((value) => value + blocks.length - 1 - seen);
      }
      lastBlock.current = last;
    }
  });

  useLayoutEffect(() => {
    if (!box || !column || box !== scroller.current) return;
    const settle = () => {
      if (!restore()) reconcile();
    };
    const watch = new ResizeObserver(settle);
    const mutations = new MutationObserver((records) => {
      settle();
      if (records.some((record) => record.type === "childList")) observe();
    });
    const readiness = new MutationObserver(settle);
    observer.current = watch;
    watched.current = new Set();
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
    // One anchor pick per painted frame, however many scroll events land.
    let frame = 0;
    const wheel = (event: WheelEvent) => {
      wheelAt.current = performance.now();
      // The ancestor walk reads layout; skip it once already unfollowed.
      if (event.deltaY < 0) {
        if (following.current && scrollsUp(event.target, box)) stopFollowing();
      } else if (event.deltaY > 0) resumeAtEnd();
    };
    const touchStart = (event: TouchEvent) => {
      touchY = event.touches[0]?.clientY || 0;
    };
    const touchMove = (event: TouchEvent) => {
      const next = event.touches[0]?.clientY || touchY;
      if (next > touchY + MOVE) {
        if (following.current && scrollsUp(event.target, box)) stopFollowing();
      } else if (next < touchY - MOVE) resumeAtEnd();
      touchY = next;
    };
    const keyDown = (event: KeyboardEvent) => {
      if (
        event.target instanceof HTMLElement &&
        event.target.closest("input,textarea,[contenteditable]")
      )
        return;
      if (["ArrowUp", "PageUp", "Home"].includes(event.key)) stopFollowing();
      else if (["ArrowDown", "PageDown", "End", " "].includes(event.key))
        resumeAtEnd();
    };
    const scroll = () => {
      // Checked before any waiting compensation: the reader's downward
      // scroll into the end must win even while a wheel is still landing.
      if (box.scrollTop > lastTop.current + MOVE && resumeAtEnd()) {
        lastTop.current = box.scrollTop;
        return;
      }
      // The wheel's scroll is still landing: wait until it goes quiet, and
      // keep the anchor the layout change moved so its shift is compensated.
      if (deferred.current !== null) {
        clearTimeout(deferred.current);
        deferred.current = setTimeout(settleWheel, WHEEL_QUIET_MS);
        lastTop.current = box.scrollTop;
        return;
      }
      const top = box.scrollTop;
      const maximum = Math.max(0, box.scrollHeight - box.clientHeight);
      if (following.current) {
        if (maximum - top > BOTTOM_BAND && top < lastTop.current - MOVE)
          stopFollowing();
      } else {
        frame ||= requestAnimationFrame(() => {
          frame = 0;
          // Following again, or a session switch awaiting its restore, since
          // the scroll: this pick would describe a state that is gone.
          if (following.current || pending.current) return;
          selectAnchor();
          observe();
        });
      }
      lastTop.current = box.scrollTop;
    };
    // Capture: the wheel is recorded before anything it triggers (a layout
    // change, its mutation callbacks) can ask for compensation.
    box.addEventListener("wheel", wheel, { passive: true, capture: true });
    box.addEventListener("touchstart", touchStart, { passive: true });
    box.addEventListener("touchmove", touchMove, { passive: true });
    box.addEventListener("keydown", keyDown);
    box.addEventListener("scroll", scroll, { passive: true });
    return () => {
      cancelAnimationFrame(frame);
      watch.disconnect();
      mutations.disconnect();
      readiness.disconnect();
      if (observer.current === watch) observer.current = null;
      box.removeEventListener("wheel", wheel, { capture: true });
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
    resumeAtEnd,
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
    preserveWhile,
    jumpToLatest,
    unseen,
  };
}
