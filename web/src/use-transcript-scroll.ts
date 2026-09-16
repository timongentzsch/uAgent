import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
} from "preact/hooks";
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
// for ARTICLE and DETAILS tool rows). This hook only owns the
// bottom stick, so there is exactly one writer per direction.
//
// Signals:
//   session switch   — resumes the saved scroll position for a known
//                      surface (fresh surfaces pin to the end); the chat
//                      surface remounts per session, so a stale surface
//                      can never rewrite the live one once effects
//                      re-attach. The switch forces one synchronous
//                      re-render so listeners/observers move from the
//                      detached node to the live one before paint; a
//                      restore that lands on the skeleton fallback stays
//                      armed until a live node is present.
//   scroll listener  — the ONLY writer that clears sticky (scrollTop moved
//                      up since the last event); re-arms sticky inside the
//                      bottom band.
//   layout pin       — useLayoutEffect after every render pins before
//                      paint, so streaming never shows a detached frame.
//   content observer — one ResizeObserver on the column pins synchronously
//                      (pre-paint) while sticky when async markdown/diagrams/
//                      images/fonts grow the transcript without a render.
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
  sessionKey: string,
) {
  const sticky = useRef(true);
  const lastTop = useRef(0);
  // Scroll positions are per surface: returning to a conversation
  // resumes where the reader left it instead of jumping to the end.
  const positions = useRef(new Map<string, number>());
  const keyRef = useRef(sessionKey);
  // Session the restore is still waiting for: the lazy chat shell can
  // mount without an app render, so a switch that lands on the skeleton
  // fallback must not lose the saved position.
  const armed = useRef<string | null>(null);
  const [, setTick] = useState(0);
  const onFollowRef = useRef(onFollow);
  onFollowRef.current = onFollow;
  // Last measured content size the bottom pin was computed from. The
  // every-render pin below only fires when the CONTENT changed: box-only
  // changes (composer wrap growth, drawer) belong to the box observer,
  // which pins post-layout with real sizes. Pinning pre-paint against a
  // stale box while content-visibility re-estimates rows above is what
  // read as the transcript glitching up and down while typing.
  const lastPin = useRef<{ node: unknown; height: number }>({
    node: null,
    height: -1,
  });

  const pinToBottom = useCallback(() => {
    const element = scroller.current;
    if (!element) return;
    lastPin.current = { node: element, height: element.scrollHeight };
    const target = element.scrollHeight - element.clientHeight;
    if (element.scrollTop !== target) element.scrollTop = target;
    lastTop.current = element.scrollTop;
  }, [scroller]);

  // True when the content swapped or grew since the last pin: the only
  // cases where a pre-paint pin measures final sizes.
  const contentPinDue = useCallback(() => {
    const element = scroller.current;
    return (
      !!element &&
      (element !== lastPin.current.node ||
        element.scrollHeight !== lastPin.current.height)
    );
  }, [scroller]);

  const setSticky = useCallback(
    (value: boolean) => {
      const element = scroller.current;
      if (element) {
        element.style.overflowAnchor = value ? "none" : "auto";
        // Live-edge flag for the stylesheet: while following, the last
        // rows lay out at real sizes (see message.css) so the bottom pin
        // measures exact heights instead of chasing content-visibility
        // estimates while typing or streaming.
        element.classList.toggle("is-following", value);
      }
      if (sticky.current === value) return;
      sticky.current = value;
      onFollowRef.current(value);
    },
    [scroller],
  );

  const stopFollowing = useCallback(() => setSticky(false), [setSticky]);

  // Restore a saved position onto the live node. Clamp to the current
  // maximum so a surface that shrank (history window, cap) pins to its
  // end instead of stranding the reader above it. Landing inside the
  // bottom band re-arms the stick, so returning to the end hides Jump
  // to latest without waiting for the next scroll event.
  const restore = useCallback(
    (saved: number) => {
      const element = scroller.current;
      if (!element) return false;
      const max = Math.max(0, element.scrollHeight - element.clientHeight);
      const target = Math.min(Math.max(0, saved), max);
      if (element.scrollTop !== target) element.scrollTop = target;
      lastTop.current = target;
      setSticky(max - target <= STICK_PX);
      return true;
    },
    [scroller, setSticky],
  );

  // Session switch resumes the saved position (or pins a fresh surface
  // to the end). The chat surface remounts per session (key in app.tsx),
  // so a stale surface can never rewrite the live one once the
  // element-bound effects below re-attach (forced below). Positions are
  // recorded continuously by the scroll listener (pins included:
  // programmatic scrolls fire scroll events), so there is nothing to
  // save here: reading a fresh node would clobber the old surface's
  // record with zero.
  useLayoutEffect(() => {
    keyRef.current = sessionKey;
    const saved = positions.current.get(sessionKey);
    if (saved !== undefined) {
      // Known surface: never auto-pin. Restore now when the remounted
      // node is already attached; otherwise stay armed and complete the
      // restore on the next render with a live node.
      setSticky(false);
      armed.current = restore(saved) ? null : sessionKey;
    } else {
      armed.current = null;
      setSticky(true);
      pinToBottom();
    }
    // Remounting the transcript swaps the node without notifying the
    // element-bound effects: ref mutation renders nothing, so their deps
    // still hold the detached node. Force one synchronous re-render so
    // listeners, observers and anchor mode re-attach to the live node
    // before paint. Without this a scroll dispatched on the detached
    // surface still reaches the old listener and rewrites the new
    // surface's stick and saved position.
    setTick((n) => n + 1);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sessionKey]);

  // After every render (new blocks, streaming deltas, session switch):
  // complete a still-armed restore first, then pin before paint while
  // sticky — but only when the content itself changed (see lastPin).
  // Streaming never shows a detached frame (deltas always change the
  // height); box-only renders defer to the box observer's post-layout
  // pin with real sizes.
  useLayoutEffect(() => {
    const pending = armed.current;
    if (pending !== null && pending === keyRef.current) {
      const saved = positions.current.get(pending);
      if (saved !== undefined && restore(saved)) armed.current = null;
    }
    if (sticky.current && contentPinDue()) pinToBottom();
  });

  // Own the anchor mode from the start: sticky begins true. On remount
  // (session switch) respect the restored stick instead of forcing none.
  useEffect(() => {
    const element = scroller.current;
    if (element) {
      element.style.overflowAnchor = sticky.current ? "none" : "auto";
      element.classList.toggle("is-following", sticky.current);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scroller.current]);

  // Scroll is the only unstick signal. A decrease in scrollTop past the
  // bottom band is always the user: pins only move down, growth fires
  // no event, and a shrink that clamps the viewport to the end keeps
  // the reader inside the band (late hydration settling must not break
  // the stick it is still serving). Bottom-band contact re-arms the
  // stick whichever way the user arrived.
  useEffect(() => {
    const element = scroller.current;
    if (!element) return;
    let frame = 0;
    // The surface that owned the gesture: read at event time, not frame
    // time, so a session switch inside the frame cannot misattribute the
    // position to the new surface.
    let pendingKey = keyRef.current;
    const onScroll = () => {
      pendingKey = keyRef.current;
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        const top = element.scrollTop;
        const max = element.scrollHeight - element.clientHeight;
        if (top < lastTop.current - MOVE_PX && top < max - STICK_PX) {
          setSticky(false);
        } else if (max - top <= STICK_PX) {
          setSticky(true);
        }
        lastTop.current = top;
        positions.current.set(pendingKey, top);
      });
    };
    element.addEventListener("scroll", onScroll, { passive: true });
    return () => {
      element.removeEventListener("scroll", onScroll);
      if (frame) cancelAnimationFrame(frame);
    };
    // scroller.current re-reads on purpose: remounting swaps the node.
    // sessionKey re-subscribes even when the node identity survives.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scroller.current, sessionKey, setSticky]);

  // Late layout (async markdown, mermaid, images, web fonts) grows the
  // content column without a render or scroll event. Compensate
  // synchronously inside the observer callback: ResizeObserver fires
  // pre-paint, so an immediate scrollTop write never shows a detached
  // frame. (Deferring to rAF used to leave one jumped frame per hydration
  // wave, which read as judder for seconds after reload while retained
  // markdown hydrated in staggered passes.) Setting scrollTop never
  // resizes content, so this cannot self-trigger. The scroller box itself
  // is observed too: composer growth, the drawer and rotation move the
  // bottom without touching the transcript.
  useEffect(() => {
    const column = content.current;
    const box = scroller.current;
    if (typeof ResizeObserver === "undefined") return;
    const observer = new ResizeObserver(() => {
      if (sticky.current) pinToBottom();
    });
    if (column) observer.observe(column);
    if (box) observer.observe(box);
    return () => observer.disconnect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [content.current, scroller.current, pinToBottom]);

  // Web-font swaps change row heights with no render and no resize of
  // the column ancestors the observer already covered mid-swap. Re-pin
  // once fonts settle while sticky.
  useEffect(() => {
    let active = true;
    document.fonts?.ready
      .then(() => {
        if (active && sticky.current) pinToBottom();
      })
      .catch(() => {});
    return () => {
      active = false;
    };
  }, [pinToBottom]);

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
