import { useCallback, useLayoutEffect, useRef, useState } from "preact/hooks";
import {
  bookmarks,
  storeBookmark,
  type Bookmark,
} from "./transcript-bookmarks.ts";
import { useLatest } from "../shared/use-latest.ts";

// Scrolling for the transcript: following the end, keeping the reader's row
// in place while history loads or rows change height, and restoring a
// position when a conversation is reopened.

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
// then for the scrolling to go quiet. Quiet is counted in painted frames: a
// busy machine stretches the scroll and this wait alike, where a fixed time
// ends mid-scroll.
const WHEEL_SETTLE_MS = 250;
const WHEEL_SETTLE_FRAMES = 15;
const WHEEL_QUIET_FRAMES = 5;

// Runs `then` after `count` painted frames; the result cancels it.
function afterFrames(count: number, then: () => void) {
  let frame = requestAnimationFrame(function tick() {
    if (--count > 0) frame = requestAnimationFrame(tick);
    else then();
  });
  return () => cancelAnimationFrame(frame);
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

const isRow = (node: Element | null | undefined): node is HTMLElement =>
  node instanceof HTMLElement && !!node.dataset.messageId;

// The first row showing in the box. Rows stack in order, so the search starts
// at `from`, the row picked last, and steps to the first row that ends below
// the top edge; without `from` it scans from the first row.
function visibleAnchor(
  box: HTMLElement,
  column: HTMLElement,
  from?: Element | null,
): Anchor | null {
  const top = box.getBoundingClientRect().top + box.clientTop;
  const bottom = top + box.clientHeight;
  const above = (row: Element) => row.getBoundingClientRect().bottom <= top + 1;
  let child = from?.parentElement === column ? from : column.firstElementChild;
  while (child && (!isRow(child) || above(child)))
    child = child.nextElementSibling;
  for (
    let prior = child?.previousElementSibling;
    prior && !(isRow(prior) && above(prior));
    prior = prior.previousElementSibling
  )
    if (isRow(prior)) child = prior;
  if (!isRow(child) || child.getBoundingClientRect().top >= bottom) return null;
  let node = child;
  for (const inner of child.querySelectorAll<HTMLElement>("[data-anchor-id]")) {
    const area = inner.getBoundingClientRect();
    if (area.bottom > top + 1 && area.top < bottom) {
      node = inner;
      break;
    }
  }
  const nodeRect = node.getBoundingClientRect();
  return {
    node,
    message: child.dataset.messageId!,
    inner: node === child ? undefined : node.dataset.anchorId,
    contentY: contentY(node, box),
    height: nodeRect.height,
    within: Math.max(0, top - nodeRect.top),
  };
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
  nodes: readonly { id: string }[],
) {
  const scroller = useRef<HTMLDivElement>(null);
  const content = useRef<HTMLDivElement>(null);
  const [box, setBox] = useState<HTMLDivElement | null>(null);
  const [column, setColumn] = useState<HTMLDivElement | null>(null);
  const following = useRef(true);
  // A deliberate jump wins over scroll events left by the previous gesture.
  // Only fresh input can detach it again.
  const jumped = useRef(false);
  const anchor = useRef<Anchor | null>(null);
  const key = useRef(sessionKey);
  const pending = useRef<string | null>(sessionKey);
  const lastTop = useRef(0);
  // The newest node seen: unseen counts only nodes appended after it, so
  // an older history page prepended above never reads as new.
  const lastNode = useRef(nodes.at(-1)?.id);
  const [unseen, setUnseen] = useState(0);
  const onFollowRef = useLatest(onFollow);
  const observer = useRef<ResizeObserver | null>(null);
  // What the observer watches, so a refresh observes only new nodes: each
  // observe() makes the observer report that node once more.
  const watched = useRef(new Set<Element>());
  const wheelAt = useRef(-Infinity);
  // A compensation waiting for the wheel's scroll to land.
  const deferred = useRef<(() => void) | null>(null);

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

  // True when the pick moved to another row.
  const selectAnchor = useCallback(() => {
    const element = scroller.current;
    const children = content.current;
    if (!element || !children) return false;
    const from = anchor.current && rowFor(anchor.current.node);
    anchor.current = visibleAnchor(element, children, from);
    capture();
    return (anchor.current && rowFor(anchor.current.node)) !== from;
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
    deferred.current?.();
    deferred.current = null;
    setFollow(true);
    pin();
    return true;
  }, [pin, setFollow]);

  // Applies a waiting compensation once the wheel's scroll has gone quiet,
  // or never started.
  const settleWheel = useCallback(() => {
    if (deferred.current === null) return;
    deferred.current();
    deferred.current = null;
    wheelAt.current = -Infinity;
    reconcileRef.current();
  }, []);

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
      deferred.current ??= afterFrames(WHEEL_SETTLE_FRAMES, settleWheel);
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
  }, [capture, pin, selectAnchor, settleWheel, writeTop]);

  const reconcileRef = useLatest(reconcile);

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
    jumped.current = false;
    anchor.current = null;
    lastNode.current = nodes.at(-1)?.id;
    setUnseen(0);
  }, [sessionKey, nodes, capture]);

  // React commits and late browser layout use the same reconciliation path.
  useLayoutEffect(() => {
    if (!box?.isConnected || box !== scroller.current || !column) return;
    if (!restore()) reconcile();
    const last = nodes.at(-1)?.id;
    if (last !== lastNode.current) {
      if (!following.current) {
        const seen = nodes.findLastIndex(
          (node) => node.id === lastNode.current,
        );
        if (seen >= 0) setUnseen((value) => value + nodes.length - 1 - seen);
      }
      lastNode.current = last;
    }
  });

  useLayoutEffect(() => {
    if (!box || !column || box !== scroller.current) return;
    const settle = () => {
      if (!restore()) reconcile();
    };
    const watch = new ResizeObserver(settle);
    // The watched set changes only with the column's rows, or with the
    // inner anchors of the row read from; markdown filling a row elsewhere
    // leaves it as it is.
    const mutations = new MutationObserver((records) => {
      settle();
      const row = anchor.current && rowFor(anchor.current.node);
      if (
        records.some(
          (record) =>
            record.type === "childList" &&
            (record.target === column || row?.contains(record.target)),
        )
      )
        observe();
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
      jumped.current = false;
      wheelAt.current = performance.now();
      // The ancestor walk reads layout; skip it once already unfollowed.
      if (event.deltaY < 0) {
        if (following.current && scrollsUp(event.target, box)) stopFollowing();
      } else if (event.deltaY > 0) resumeAtEnd();
    };
    const touchStart = (event: TouchEvent) => {
      jumped.current = false;
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
      const up = ["ArrowUp", "PageUp", "Home"].includes(event.key);
      if (up || ["ArrowDown", "PageDown", "End", " "].includes(event.key)) {
        jumped.current = false;
        if (up) stopFollowing();
        else resumeAtEnd();
      }
    };
    const pointerDown = () => {
      jumped.current = false;
    };
    const scroll = () => {
      if (jumped.current && following.current) {
        pin();
        return;
      }
      // Checked before any waiting compensation: the reader's downward
      // scroll into the end must win even while a wheel is still landing.
      if (box.scrollTop > lastTop.current + MOVE && resumeAtEnd()) {
        lastTop.current = box.scrollTop;
        return;
      }
      // The wheel's scroll is still landing: wait until it goes quiet, and
      // keep the anchor the layout change moved so its shift is compensated.
      if (deferred.current !== null) {
        deferred.current();
        deferred.current = afterFrames(WHEEL_QUIET_FRAMES, settleWheel);
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
          // The rows are watched as they change; a new anchor row adds its
          // inner anchors.
          if (selectAnchor()) observe();
        });
      }
      lastTop.current = box.scrollTop;
    };
    // Capture: the wheel is recorded before anything it triggers (a layout
    // change, its mutation callbacks) can ask for compensation.
    box.addEventListener("wheel", wheel, { passive: true, capture: true });
    box.addEventListener("pointerdown", pointerDown, { passive: true });
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
      box.removeEventListener("pointerdown", pointerDown);
      box.removeEventListener("touchstart", touchStart);
      box.removeEventListener("touchmove", touchMove);
      box.removeEventListener("keydown", keyDown);
      box.removeEventListener("scroll", scroll);
    };
  }, [
    box,
    column,
    observe,
    pin,
    reconcile,
    restore,
    resumeAtEnd,
    selectAnchor,
    settleWheel,
    stopFollowing,
  ]);

  const jumpToLatest = useCallback(() => {
    pending.current = null;
    jumped.current ||= !following.current;
    deferred.current?.();
    deferred.current = null;
    wheelAt.current = -Infinity;
    // Reset the native scroll container to cancel touch inertia before pinning.
    const element = scroller.current;
    if (element) {
      const overflow = element.style.overflowY;
      element.style.overflowY = "hidden";
      void element.offsetHeight;
      element.style.overflowY = overflow;
    }
    setFollow(true);
    pin();
    capture();
  }, [capture, pin, setFollow]);

  // Puts a found row or words in the middle of the view and stops
  // following, so a streamed row does not pull the view away from it.
  const reveal = useCallback(
    (target: Element | Range) => {
      const element = scroller.current;
      if (!element) return;
      pending.current = null;
      jumped.current = true;
      stopFollowing();
      const view = element.getBoundingClientRect();
      const box = target.getBoundingClientRect();
      writeTop(
        element.scrollTop +
          box.top -
          view.top -
          (element.clientHeight - box.height) / 2,
      );
      selectAnchor();
    },
    [selectAnchor, stopFollowing, writeTop],
  );

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
    attachScroller,
    attachContent,
    preserveWhile,
    jumpToLatest,
    reveal,
    unseen,
  };
}
