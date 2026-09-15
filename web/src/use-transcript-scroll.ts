import { useCallback, useEffect, useLayoutEffect, useRef } from "preact/hooks";
import type { RefObject } from "preact";

// Distance (px) from the bottom that still counts as "at the end". Catches
// the streaming case where a frame lands a few pixels shy of the maximum
// while the user never scrolled up.
const STICK_PX = 100;
// Ignore sub-pixel and rubber-band noise when judging scroll direction.
const MOVE_PX = 2;

// Stick-to-bottom owner, built around one invariant:
//
//   Only the user can break the stick, and only by moving UP.
//
// Content growth never fires scroll events, so it can never break the
// stick. Programmatic pins only ever move DOWN, so comparing scrollTop
// across scroll events unambiguously separates user intent from our own
// pins. (A sentinel IntersectionObserver cannot do this: its callback is
// async post-layout, so fast growth during a turn unpins between the
// observe fire and the pin. That race was the old unreliability.)
//
// While sticky, native overflow-anchoring is disabled on the scroller so
// the manual pin owns the bottom uncontested (anchor adjustments during
// the initial burst and streaming frames caused the first-seconds
// judder); while reading history it is enabled so prepends and late
// mutations hold position.
//
// Prepend anchoring lives in chat.tsx (single owner, rect-based, works
// for ARTICLE and exploration DETAILS rows). This hook only owns the
// bottom stick, so there is exactly one writer per direction.
//
// Signals:
//   scroll listener  — the ONLY writer that clears sticky (scrollTop moved
//                      up since the last event); re-arms sticky inside the
//                      bottom band.
//   layout pin       — useLayoutEffect after every render pins before
//                      paint, so streaming never shows a detached frame.
//   content observer — one ResizeObserver on the column re-pins while
//                      sticky when async markdown/diagrams/images/fonts
//                      grow the transcript without a render.
//   box observer     — one ResizeObserver on the scroller itself re-pins
//                      while sticky when the box changes (composer growth,
//                      drawer, rotation, devtools) without any render.
//   viewport         — window/visualViewport resize+scroll (mobile
//                      keyboard) re-pin while sticky, twice deferred so
//                      layout has settled.
//   focusin          — focusing the composer re-pins while sticky,
//                      countering scroll-into-view ancestor scrolling.
// No sentinel observer, no MutationObserver, no gesture listeners.
export function useTranscriptScroll(
  scroller: RefObject<HTMLDivElement>,
  content: RefObject<HTMLDivElement>,
  onFollow: (following: boolean) => void,
) {
  const sticky = useRef(true);
  const lastTop = useRef(0);
  const onFollowRef = useRef(onFollow);
  onFollowRef.current = onFollow;

  const pinToBottom = useCallback(() => {
    const element = scroller.current;
    if (!element) return;
    const target = element.scrollHeight - element.clientHeight;
    if (element.scrollTop !== target) element.scrollTop = target;
    lastTop.current = element.scrollTop;
  }, [scroller]);

  const setSticky = useCallback(
    (value: boolean) => {
      const element = scroller.current;
      if (element)
        element.style.overflowAnchor = value ? "none" : "auto";
      if (sticky.current === value) return;
      sticky.current = value;
      onFollowRef.current(value);
    },
    [scroller],
  );

  const stopFollowing = useCallback(() => setSticky(false), [setSticky]);

  // After every render (new blocks, streaming deltas, session switch):
  // pin before paint while sticky so no frame shows a detached bottom.
  useLayoutEffect(() => {
    if (sticky.current) pinToBottom();
  });

  // Own the anchor mode from the start: sticky begins true.
  useEffect(() => {
    const element = scroller.current;
    if (element) element.style.overflowAnchor = "none";
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scroller.current]);

  // Scroll is the only unstick signal. A decrease in scrollTop is always
  // the user (pins only move down; growth fires no event). Bottom-band
  // contact re-arms the stick whichever way the user arrived.
  useEffect(() => {
    const element = scroller.current;
    if (!element) return;
    let frame = 0;
    const onScroll = () => {
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        const top = element.scrollTop;
        if (top < lastTop.current - MOVE_PX) {
          setSticky(false);
        } else if (
          element.scrollHeight - top - element.clientHeight <=
          STICK_PX
        ) {
          setSticky(true);
        }
        lastTop.current = top;
      });
    };
    element.addEventListener("scroll", onScroll, { passive: true });
    return () => {
      element.removeEventListener("scroll", onScroll);
      if (frame) cancelAnimationFrame(frame);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scroller.current, setSticky]);

  // Late layout (async markdown, mermaid, images, web fonts) grows the
  // content column without a render or scroll event. Re-pin while sticky;
  // setting scrollTop never resizes content, so this cannot self-trigger.
  // The scroller box itself is observed too: composer growth, the drawer
  // and rotation move the bottom without touching the transcript.
  useEffect(() => {
    const column = content.current;
    const box = scroller.current;
    if (typeof ResizeObserver === "undefined") return;
    let frame = 0;
    const repin = () => {
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        if (sticky.current) pinToBottom();
      });
    };
    const observer = new ResizeObserver(repin);
    if (column) observer.observe(column);
    if (box) observer.observe(box);
    return () => {
      observer.disconnect();
      if (frame) cancelAnimationFrame(frame);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [content.current, scroller.current, pinToBottom]);

  // Window and visual-viewport (mobile keyboard) resizes and viewport
  // scrolls move the bottom without scrolling the transcript. Re-pin
  // while sticky, twice deferred: mobile browsers report the resize
  // before layout settles, and the first frame still measures stale.
  // Focus is deferred the same way: iOS Safari scrolls ancestors into
  // view on focus before the keyboard geometry arrives.
  useEffect(() => {
    const repin = () => {
      if (!sticky.current) return;
      requestAnimationFrame(() =>
        requestAnimationFrame(() => {
          if (sticky.current) pinToBottom();
        }),
      );
    };
    window.addEventListener("resize", repin);
    document.addEventListener("focusin", repin);
    const viewport = window.visualViewport;
    viewport?.addEventListener("resize", repin);
    viewport?.addEventListener("scroll", repin);
    return () => {
      window.removeEventListener("resize", repin);
      document.removeEventListener("focusin", repin);
      viewport?.removeEventListener("resize", repin);
      viewport?.removeEventListener("scroll", repin);
    };
  }, [pinToBottom]);

  // Synchronous and idempotent: the first press pins and hides the Jump
  // button in the same frame (following flips true), so a second press
  // is impossible. Content still arriving (jump right after reload) is
  // held by the layout pin and content observer while sticky is true.
  const jumpToLatest = useCallback(() => {
    setSticky(true);
    pinToBottom();
  }, [pinToBottom, setSticky]);

  return { jumpToLatest, stopFollowing };
}
