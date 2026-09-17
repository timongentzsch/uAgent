import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
} from "preact/hooks";

// Distance (px) from the bottom that still counts as "at the end". Catches
// the streaming case where a frame lands a few pixels shy of the maximum
// while the user never scrolled up.
const STICK_PX = 100;
// True when this page lifetime started with a reload: only then can the
// browser restore a stale pre-reload position onto the fresh transcript
// node (same-document switches have no browser involvement, and fresh
// links carry no persisted state).
const RELOADED =
  typeof performance !== "undefined" &&
  typeof performance.getEntriesByType === "function" &&
  (
    performance.getEntriesByType("navigation")[0] as
      PerformanceNavigationTiming | undefined
  )?.type === "reload";
// Ignore sub-pixel and rubber-band noise when judging scroll direction.
const MOVE_PX = 2;
// Coalescing window (ms) for shrink evidence below. WebKit dispatches
// scroll events asynchronously: a shrink-clamp's event can arrive after
// regrowth already restored the maximum, erasing the size evidence the
// fast path checks. The low-water mark below survives that.
const SHRINK_WINDOW_MS = 1000;

// Single writer for the scroller's anchor mode: manual pins own the
// bottom while sticky (native adjustments during the initial burst and
// streaming frames judder); native anchoring holds position while
// reading history. Chat's prepend restore borrows this so there is one
// function owning both DOM writes.
export function setAnchorMode(element: HTMLElement | null, sticky: boolean) {
  if (!element) return;
  element.style.overflowAnchor = sticky ? "none" : "auto";
  // Live-edge flag for the stylesheet: while following, the last rows
  // lay out at real sizes (see message.css) so the bottom pin measures
  // exact heights instead of chasing content-visibility estimates.
  element.classList.toggle("is-following", sticky);
}

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
// Nodes arrive through attach callbacks, so listeners and observers
// rebind on every session remount with no forced re-render: the refs
// below are stable mirrors for imperative reads, the state pair drives
// the node-keyed effects.
//
// Signals:
//   session switch   — resumes the saved scroll position for a known
//                      surface (fresh surfaces pin to the end); the settle
//                      waits for the live node instead of a render tick.
//   scroll listener  — synchronously clears sticky on any upward move
//                      (zero-delay evidence: the only unstick writer);
//                      a deferred frame re-arms sticky on downward
//                      arrival inside the bottom band and records the
//                      position. The frame never unsticks: silent
//                      shrink-clamps between event and frame would read
//                      as user intent while the reader sits at bottom.
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
export function useStickToBottom(
  onFollow: (following: boolean) => void,
  sessionKey: string,
  arrivals: number,
) {
  const sticky = useRef(true);
  const lastTop = useRef(0);
  // Previous frame's maximum scroll offset. Estimates resolving
  // (content-visibility, skeletons, fonts) can shrink the content so
  // far that the browser clamps the viewport down to the new end: an
  // upward move nobody chose. Comparing maxima across frames tells
  // that clamp apart from real scroll-aways (see the frame below).
  const lastFrameMax = useRef(0);
  // Low-water mark of the scrollable maximum: the smallest maximum seen
  // recently, with its timestamp. A shrink-then-regrow burst (estimate
  // resolution under arriving rows) clamps the viewport to the shrunken
  // end; when WebKit delivers that clamp's scroll event late, the live
  // maximum already recovered and only this mark still proves the clamp.
  // Consumed on forgive (one clamp, one forgiveness); shrinks re-arm it
  // in the content observer, so a continued genuine gesture always
  // breaks on its next event.
  const lowWater = useRef<{ max: number; at: number }>({
    max: Infinity,
    at: 0,
  });
  // resumes where the reader left it instead of jumping to the end.
  const positions = useRef(new Map<string, number>());
  const keyRef = useRef(sessionKey);
  // Settle job for the next live node: { top } restores a saved
  // position, { top: null } pins a fresh surface to the end.
  const pending = useRef<{ key: string; top: number | null } | null>(null);
  const arrivalsRef = useRef(arrivals);
  arrivalsRef.current = arrivals;
  const arrivalsBaseline = useRef(arrivals);
  const [unseen, setUnseen] = useState(0);
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

  const scroller = useRef<HTMLDivElement>(null);
  const content = useRef<HTMLDivElement>(null);
  // Armed by history traversal (back/forward): pushState switches never
  // fire these, reloads start a fresh lifetime, so this is true only
  // when the browser may land its own post-traversal scroll after our
  // pre-paint restore. Consumed once by the next settled restore.
  const traversalArmed = useRef(false);
  useEffect(() => {
    const arm = () => {
      traversalArmed.current = true;
    };
    window.addEventListener("popstate", arm);
    window.addEventListener("hashchange", arm);
    return () => {
      window.removeEventListener("popstate", arm);
      window.removeEventListener("hashchange", arm);
    };
  }, []);
  // Reload-fresh surface key for the post-load settle below: set when a
  // fresh surface mounts after a reload, consumed once the load event
  // (or an already-complete document) lets us converge past the
  // browser's own restoration work.
  const reloadPin = useRef<string | null>(null);
  const [box, setBox] = useState<HTMLDivElement | null>(null);
  const [column, setColumn] = useState<HTMLDivElement | null>(null);
  const attachScroller = useCallback((node: HTMLDivElement | null) => {
    scroller.current = node;
    setBox(node);
  }, []);
  const attachContent = useCallback((node: HTMLDivElement | null) => {
    content.current = node;
    setColumn(node);
  }, []);

  const pinToBottom = useCallback(() => {
    const element = scroller.current;
    if (!element) return;
    lastPin.current = { node: element, height: element.scrollHeight };
    const target = element.scrollHeight - element.clientHeight;
    if (element.scrollTop !== target) element.scrollTop = target;
    lastTop.current = element.scrollTop;
  }, []);

  // True when the content swapped or grew since the last pin: the only
  // cases where a pre-paint pin measures final sizes.
  const contentPinDue = useCallback(() => {
    const element = scroller.current;
    return (
      !!element &&
      (element !== lastPin.current.node ||
        element.scrollHeight !== lastPin.current.height)
    );
  }, []);

  const setSticky = useCallback((value: boolean) => {
    const element = scroller.current;
    setAnchorMode(element, value);
    if (sticky.current === value) return;
    sticky.current = value;
    if (value) setUnseen(0);
    onFollowRef.current(value);
  }, []);

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
      // Defer while the surface is still loading: clamping to a
      // transient skeleton max would consume the job at 0 and strand
      // the reader there once the real rows arrive. The every-render
      // effect retries on the next render, so the deferred job still
      // settles within the same switch once content lands.
      if (element.getAttribute("aria-busy") !== null && max < saved)
        return false;
      const target = Math.min(Math.max(0, saved), max);
      if (element.scrollTop !== target) element.scrollTop = target;
      lastTop.current = target;
      setSticky(max - target <= STICK_PX);
      return true;
    },
    [setSticky],
  );

  // Session switch resumes the saved position (or pins a fresh surface
  // to the end) via a settle job; the node-keyed effect below runs it
  // once the live node exists. Positions are recorded continuously by
  // the scroll listener (pins included: programmatic scrolls fire
  // scroll events), so there is nothing to save here.
  useLayoutEffect(() => {
    keyRef.current = sessionKey;
    setUnseen(0);
    arrivalsBaseline.current = arrivalsRef.current;
    const saved = positions.current.get(sessionKey);
    // Seed the direction comparator for the incoming surface: without
    // this the first scroll on the new surface judges against the old
    // surface's last top and misreads intent.
    lastTop.current = saved ?? 0;
    if (saved !== undefined) {
      // Known surface: never auto-pin.
      setSticky(false);
      pending.current = { key: sessionKey, top: saved };
    } else {
      if (RELOADED) reloadPin.current = sessionKey;
      setSticky(true);
      pending.current = { key: sessionKey, top: null };
    }
  }, [sessionKey]);

  // After every render: settle a pending switch job on the live node,
  // then pin before paint while sticky — but only when the content
  // itself changed (see lastPin). The isConnected guard is the crux:
  // across a remount commit the state still holds the detached node,
  // and settling there would consume the job without ever restoring
  // the live surface. Arrivals landing here while unfollowed accumulate
  // the unseen badge; unsettled surfaces never count.
  useLayoutEffect(() => {
    const job = pending.current;
    if (job && box && box.isConnected && job.key === keyRef.current) {
      if (job.top === null) {
        pinToBottom();
        pending.current = null;
      } else if (restore(job.top)) {
        pending.current = null;
        // History traversal (but never clicks: pushState fires no
        // popstate/hashchange) lets the browser restore its own scroll
        // after our pre-paint restore, silently moving the reader off
        // the resumed spot. Re-assert once past that work: a no-op
        // when nothing moved, so it can never flicker or fight intent.
        if (traversalArmed.current) {
          traversalArmed.current = false;
          const key = job.key;
          const top = job.top;
          requestAnimationFrame(() =>
            requestAnimationFrame(() => {
              const element = scroller.current;
              if (!element || !element.isConnected) return;
              if (pending.current || keyRef.current !== key) return;
              const max = Math.max(
                0,
                element.scrollHeight - element.clientHeight,
              );
              const target = Math.min(Math.max(0, top), max);
              if (Math.abs(element.scrollTop - target) > MOVE_PX) {
                element.scrollTop = target;
                lastTop.current = element.scrollTop;
              }
            }),
          );
        }
      } else {
        if (pending.current) arrivalsBaseline.current = arrivals;
        return;
      }
      lastFrameMax.current = Math.max(0, box.scrollHeight - box.clientHeight);
    } else if (sticky.current && contentPinDue()) {
      pinToBottom();
    }
    if (pending.current) {
      arrivalsBaseline.current = arrivals;
    } else if (arrivals > arrivalsBaseline.current) {
      if (!sticky.current)
        setUnseen((count) => count + arrivals - arrivalsBaseline.current);
      arrivalsBaseline.current = arrivals;
    } else if (arrivals < arrivalsBaseline.current) {
      arrivalsBaseline.current = arrivals;
    }
  });

  // Scroll is the only unstick signal. Breaking is directional — any
  // upward move is the user (pins only move down and growth fires no
  // event). A shrink that clamps the viewport also reads as upward,
  // so the frame below forgives clamp-driven unsticks (see
  // lastFrameMax). Re-arm is positional: arriving DOWN into the
  // bottom band re-arms, whichever surface gesture brought the reader
  // there. An explicit scroll-away is never re-armed on the way up,
  // even inside the band: intent wins over proximity.
  useEffect(() => {
    if (!box) return;
    const element = box;
    // Stale node (remount raced resubscribe: state still holds the
    // detached surface while the live one is already attached): never
    // subscribe a dead surface — its cleanup flush would otherwise
    // overwrite the live surface's saved position with stale geometry.
    if (element !== scroller.current) return;
    let frame = 0;
    // The surface that owned the gesture: read at event time, not frame
    // time, so a session switch inside the frame cannot misattribute the
    // position to the new surface.
    let pendingKey = keyRef.current;
    const onScroll = () => {
      // Stale subscription (remount raced resubscribe): a detached
      // surface must never evaluate stickiness nor rewrite the live
      // surface's saved position — drop it and self-heal.
      if (element !== scroller.current) {
        element.removeEventListener("scroll", onScroll);
        return;
      }
      pendingKey = keyRef.current;
      // Synchronous save: the remount cleanup below cancels a pending
      // frame, so a session switch racing the frame would otherwise
      // lose this position and the next restore would land elsewhere
      // (the webkit-only flake). The frame keeps its copy for re-arm.
      positions.current.set(pendingKey, element.scrollTop);
      // Fast path (and only unstick writer): an upward move is the user —
      // unless the geometry proves a clamp. Pins only move the viewport
      // down and growth fires no scroll event, so an upward delta with
      // content still below the departure point is always intent. But
      // when the content shrank under the pinned position, the browser
      // clamps the viewport up to the shrunken end: an upward move
      // nobody chose. An empty box (max <= 0, the mount skeleton swap)
      // never breaks: there is no content to have intent about, and the
      // frame below is blind there too, so judging would strand fresh
      // surfaces at the top on every slow load (measured: pin to a
      // 275px skeleton end, collapse to max 0 within 10ms). Past that,
      // WebKit can deliver a clamp's event after regrowth restored the
      // maximum, erasing the live evidence — the low-water mark covers
      // those deep landings. Everything else breaks: top landings stay
      // responsive (the frame's at-end rule still heals true end-clamps
      // once layout exists). Forgiveness consumes the mark (one clamp,
      // one forgiveness; shrinks re-arm it in the content observer), so
      // a continued genuine gesture always breaks on its next event.
      if (element.scrollTop < lastTop.current - MOVE_PX) {
        const top = element.scrollTop;
        const max = element.scrollHeight - element.clientHeight;
        const now = performance.now();
        if (now - lowWater.current.at > SHRINK_WINDOW_MS)
          lowWater.current = { max, at: now };
        const prev = lastTop.current;
        const intact =
          max >= prev - MOVE_PX && lowWater.current.max >= prev - MOVE_PX;
        const deepClamp =
          top > STICK_PX && lowWater.current.max < prev - MOVE_PX;
        if (max > 0 && (intact || !deepClamp)) setSticky(false);
        else lowWater.current = { max, at: now };
      }
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        const top = element.scrollTop;
        const max = element.scrollHeight - element.clientHeight;
        // Echoes of our own pins/restores (top exactly where we put it)
        // carry no user intent: evaluating them against mid-settle
        // layout misreads a small transient max as bottom contact and
        // re-arms the stick behind a reader we just restored. The
        // position is still recorded below, so future restores hold.
        // Degenerate layout (max <= 0: empty remount window) never
        // flips stickiness either way for the same reason.
        if (max > 0 && top !== lastTop.current) {
          // Downward into the band: arrived at the end. (Upward moves
          // already broke the stick synchronously above; re-judging them
          // here would read stale shrink-clamp deltas as user intent.)
          if (top > lastTop.current + MOVE_PX && max - top <= STICK_PX) {
            setSticky(true);
          }
        }
        // Forgive a shrink-clamp unstick: when the content shrank since
        // the last frame and this event landed at the new end, the fast
        // path above just broke the stick on a clamp (estimates
        // resolving pulled the floor up under a bottom-pinned reader),
        // not on intent. Re-stick so the pins keep converging instead
        // of stranding the reader above still-arriving content. Real
        // scroll-aways never shrink the maximum, and in-band arrivals
        // are the end by definition, so neither can trip this.
        if (
          !sticky.current &&
          max > 0 &&
          max < lastFrameMax.current - MOVE_PX &&
          max - top <= STICK_PX
        ) {
          setSticky(true);
        }
        lastTop.current = top;
        lastFrameMax.current = max;
        positions.current.set(pendingKey, top);
      });
    };
    element.addEventListener("scroll", onScroll, { passive: true });
    return () => {
      element.removeEventListener("scroll", onScroll);
      if (frame) cancelAnimationFrame(frame);
    };
  }, [box, sessionKey, setSticky]);

  // Late layout (async markdown, mermaid, images, web fonts) grows the
  // content column without a render or scroll event. Compensate
  // synchronously inside the observer callback: ResizeObserver fires
  // pre-paint, so an immediate scrollTop write never shows a detached
  // frame. Setting scrollTop never resizes content, so this cannot
  // self-trigger. The scroller box itself is observed too: composer
  // growth, the drawer and rotation move the bottom without touching
  // the transcript.
  useEffect(() => {
    if (typeof ResizeObserver === "undefined") return;
    const observer = new ResizeObserver(() => {
      const element = scroller.current;
      if (element) {
        const max = element.scrollHeight - element.clientHeight;
        if (max < lowWater.current.max)
          lowWater.current = { max, at: performance.now() };
      }
      if (sticky.current) pinToBottom();
    });
    if (column) observer.observe(column);
    if (box) observer.observe(box);
    return () => observer.disconnect();
  }, [column, box, pinToBottom]);

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

  // Post-load settle for reload-fresh surfaces: restoration lands
  // before the load event, so converging the fresh surface to the
  // bottom here runs after any stale restore the browser applied. It
  // fires once, only while the reload-fresh surface is still current
  // and settled, and writes nothing when already at the end (no event,
  // no flicker). A reader who scrolled during the unsettled load phase
  // converges too — their position predates settled content either way.
  useEffect(() => {
    if (!RELOADED) return;
    let cancelled = false;
    const settle = () => {
      if (cancelled) return;
      const key = reloadPin.current;
      reloadPin.current = null;
      if (!key || keyRef.current !== key || pending.current) return;
      pinToBottom();
    };
    if (document.readyState === "complete") {
      const frame = requestAnimationFrame(() => {
        if (!cancelled) requestAnimationFrame(settle);
      });
      return () => {
        cancelled = true;
        cancelAnimationFrame(frame);
      };
    }
    window.addEventListener("load", settle);
    return () => {
      cancelled = true;
      window.removeEventListener("load", settle);
    };
  }, [pinToBottom]);

  // Persist the end across unload: a reload wipes the in-memory
  // positions, so every surface remounts fresh and fresh surfaces pin
  // to the bottom. Without this the browser persists the current
  // position and restores it onto the new node after our mount pin,
  // fighting the pin with a stale value (the post-reload jump). The
  // in-memory map is untouched, so same-lifetime restores (bfcache
  // traversal) still resume their saved spots.
  useEffect(() => {
    const settle = () => {
      const element = scroller.current;
      if (element) pinToBottom();
    };
    window.addEventListener("pagehide", settle);
    return () => window.removeEventListener("pagehide", settle);
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
  // User-initiated jumps glide (ChatGPT convention); every programmatic
  // pin stays instant. Honors prefers-reduced-motion.
  const jumpToLatest = useCallback(() => {
    const element = scroller.current;
    setSticky(true);
    if (!element) return;
    const target = Math.max(0, element.scrollHeight - element.clientHeight);
    lastPin.current = { node: element, height: element.scrollHeight };
    if (
      typeof element.scrollTo === "function" &&
      !window.matchMedia("(prefers-reduced-motion: reduce)").matches &&
      target - element.scrollTop > STICK_PX
    ) {
      // Greet the scroll listener with the departure point, not the
      // destination: upward frames below a preset target would read as
      // the user breaking the stick mid-flight.
      lastTop.current = element.scrollTop;
      element.scrollTo({ top: target, behavior: "smooth" });
      return;
    }
    if (element.scrollTop !== target) element.scrollTop = target;
    lastTop.current = element.scrollTop;
  }, [setSticky]);

  return {
    scroller,
    content,
    attachScroller,
    attachContent,
    jumpToLatest,
    stopFollowing,
    unseen,
  };
}
