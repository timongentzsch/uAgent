import { useCallback, useEffect, useRef } from "preact/hooks";
import type { RefObject } from "preact";

// Distance (px) from the bottom that still counts as "at the end". Catches
// the streaming case where the sentinel flickers out of view for a frame
// while the user never scrolled up.
const STICK_PX = 100;

// Hybrid stick-to-bottom owner for the transcript:
//   - passive scroll listener (rAF-throttled) + sentinel observer feed one
//     sticky flag: sentinel visible OR within STICK_PX of the bottom.
//   - after every render, if sticky and not at the bottom, pin to bottom.
//   - a ResizeObserver on the content column re-pins while sticky when
//     late layout (async markdown, diagrams, images, fonts) grows the
//     transcript without a render or scroll event.
//   - resource loads (images), viewport resizes (mobile keyboard) and mount
//     re-pin when sticky.
// No MutationObserver, no per-frame DOM queries while idle.
export function useTranscriptScroll(
  scroller: RefObject<HTMLDivElement>,
  content: RefObject<HTMLDivElement>,
  sentinel: RefObject<HTMLDivElement>,
  onFollow: (following: boolean) => void,
) {
  const sticky = useRef(true);
  const followRef = useRef(true);
  const onFollowRef = useRef(onFollow);
  onFollowRef.current = onFollow;

  const distanceToBottom = useCallback(() => {
    const element = scroller.current;
    if (!element) return 0;
    return element.scrollHeight - element.scrollTop - element.clientHeight;
  }, [scroller]);

  const pinToBottom = useCallback(() => {
    const element = scroller.current;
    if (!element) return;
    const target = element.scrollHeight - element.clientHeight;
    if (element.scrollTop !== target) element.scrollTop = target;
  }, [scroller]);

  const setSticky = useCallback((value: boolean) => {
    sticky.current = value;
    if (followRef.current !== value) {
      followRef.current = value;
      onFollowRef.current(value);
    }
  }, []);

  // Recompute stickiness from the live geometry. OR-combined with the
  // sentinel signal so a single flickering source can't drop the stick.
  const refreshSticky = useCallback(() => {
    setSticky(distanceToBottom() <= STICK_PX);
  }, [distanceToBottom, setSticky]);

  const refreshStickyRef = useRef(refreshSticky);
  refreshStickyRef.current = refreshSticky;

  // After every render (new blocks, streaming deltas, session switch):
  // if the user is sticky but layout moved the bottom, pin it back.
  // Runs in layout phase so the paint never shows a detached bottom.
  useEffect(() => {
    if (!sticky.current) return;
    pinToBottom();
    // One more pin on the next frame catches late layout (web fonts,
    // image dimensions, markdown highlight) without a steady observer.
    const frame = requestAnimationFrame(() => {
      if (sticky.current) pinToBottom();
    });
    return () => cancelAnimationFrame(frame);
  });

  // Passive scroll listener: the only signal that can *break* the stick
  // (user scrolling up) and the one that re-arms it near the bottom.
  useEffect(() => {
    const element = scroller.current;
    if (!element) return;
    let frame = 0;
    const onScroll = () => {
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        refreshStickyRef.current();
      });
    };
    element.addEventListener("scroll", onScroll, { passive: true });
    return () => {
      element.removeEventListener("scroll", onScroll);
      if (frame) cancelAnimationFrame(frame);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scroller.current]);

  // Sentinel observer as a second stickiness source: intersecting means
  // sticky, but a non-intersecting sentinel must NOT clear a stick that
  // the geometry check still confirms (fast streaming growth).
  useEffect(() => {
    const root = scroller.current;
    const target = sentinel.current;
    if (!root || !target) return;
    if (typeof IntersectionObserver === "undefined") {
      setSticky(true);
      return;
    }
    const observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.target !== target) continue;
          if (entry.isIntersecting) {
            setSticky(true);
          } else {
            refreshStickyRef.current();
          }
        }
      },
      { root, threshold: 0 },
    );
    observer.observe(target);
    return () => observer.disconnect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scroller.current, sentinel.current, setSticky]);

  // Late layout (async markdown, mermaid, images, web fonts) grows the
  // content column without a render or scroll event. One observer on the
  // column re-pins while sticky; setting scrollTop never resizes content,
  // so this cannot self-trigger.
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

  // Late subresource loads (images, fonts) shift the bottom without a
  // render. Capture-phase "load" catches them; pin only when sticky.
  // Visual-viewport resizes (mobile keyboard) do the same for clientHeight.
  useEffect(() => {
    const element = scroller.current;
    if (!element) return;
    const onResource = () => {
      if (sticky.current) pinToBottom();
    };
    element.addEventListener("load", onResource, true);
    const viewport = window.visualViewport;
    const onResize = () => {
      if (sticky.current) pinToBottom();
    };
    viewport?.addEventListener("resize", onResize);
    return () => {
      element.removeEventListener("load", onResource, true);
      viewport?.removeEventListener("resize", onResize);
    };
  }, [pinToBottom, scroller]);

  const jumpToLatest = useCallback(() => {
    setSticky(true);
    pinToBottom();
    // Content may still be arriving (jump right after a reload); the
    // per-render effect keeps pinning while sticky stays true.
    requestAnimationFrame(() => {
      if (sticky.current) pinToBottom();
    });
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
      if (applied) element.scrollTop = top + (element.scrollHeight - before);
      return applied;
    },
    [scroller],
  );

  return { jumpToLatest, preservePrepend };
}
