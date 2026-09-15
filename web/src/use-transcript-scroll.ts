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
// observe fire and the pin. That race was the unreliability.)
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
//   viewport resize  — window/visualViewport (mobile keyboard) re-pin
//                      while sticky.
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

  const setSticky = useCallback((value: boolean) => {
    if (sticky.current === value) return;
    sticky.current = value;
    onFollowRef.current(value);
  }, []);

  // After every render (new blocks, streaming deltas, session switch):
  // pin before paint while sticky so no frame shows a detached bottom.
  useLayoutEffect(() => {
    if (sticky.current) pinToBottom();
  });

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
          if (sticky.current) {
            sticky.current = false;
            onFollowRef.current(false);
          }
        } else if (
          element.scrollHeight - top - element.clientHeight <=
          STICK_PX
        ) {
          if (!sticky.current) {
            sticky.current = true;
            onFollowRef.current(true);
          }
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
  }, [scroller.current]);

  // Late layout (async markdown, mermaid, images, web fonts) grows the
  // content column without a render or scroll event. Re-pin while sticky;
  // setting scrollTop never resizes content, so this cannot self-trigger.
  useEffect(() => {
    const target = content.current;
    if (!target || typeof ResizeObserver === "undefined") return;
    let frame = 0;
    const observer = new ResizeObserver(() => {
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        if (sticky.current) pinToBottom();
      });
    });
    observer.observe(target);
    return () => {
      observer.disconnect();
      if (frame) cancelAnimationFrame(frame);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [content.current, pinToBottom]);

  // Window and visual-viewport (mobile keyboard) resizes move the bottom
  // without scrolling. Re-pin while sticky.
  useEffect(() => {
    const onResize = () => {
      if (sticky.current) pinToBottom();
    };
    window.addEventListener("resize", onResize);
    const viewport = window.visualViewport;
    viewport?.addEventListener("resize", onResize);
    return () => {
      window.removeEventListener("resize", onResize);
      viewport?.removeEventListener("resize", onResize);
    };
  }, [pinToBottom]);

  const jumpToLatest = useCallback(() => {
    setSticky(true);
    pinToBottom();
    // Content may still be arriving (jump right after a reload); the
    // layout pin and content observer keep holding while sticky is true.
  }, [pinToBottom, setSticky]);

  // Prepend older pages without moving the visible anchor. Only needed where
  // native overflow-anchor is unsupported; elsewhere the browser holds position
  // and this is a harmless no-op correction.
  const preservePrepend = useCallback(
    async (load: () => Promise<boolean>) => {
      const element = scroller.current;
      if (!element) return load();
      const nativeAnchor =
        typeof CSS !== "undefined" &&
        typeof CSS.supports === "function" &&
        CSS.supports("overflow-anchor", "auto");
      if (nativeAnchor) return load();
      const before = element.scrollHeight;
      const top = element.scrollTop;
      const applied = await load();
      if (applied) {
        element.scrollTop = top + (element.scrollHeight - before);
        lastTop.current = element.scrollTop;
      }
      return applied;
    },
    [scroller],
  );

  return { jumpToLatest, preservePrepend };
}
