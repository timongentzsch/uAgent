import type { RefObject } from "preact";
import { useEffect, useLayoutEffect } from "preact/hooks";
import { coarsePointer, viewportBounds } from "./layout.ts";
import {
  edgeSwipeOpenPx,
  edgeSwipeStartPx,
  sheetCloseShare,
  sheetFlickSpeed,
  swipeSlopPx,
} from "./limits.ts";
import { useLatest } from "./use-latest.ts";

// A sheet follows a finger pulling it down from its top and closes when let
// go far or fast enough; short of that it springs back. The drag is the
// --swipe offset, which styles/primitives.css turns into its translate.
export function useSheetSwipe(
  ref: RefObject<HTMLDialogElement | null>,
  close: () => void,
  enabled: boolean,
) {
  const latest = useLatest(close);
  // Layout effect: listening as soon as the sheet is drawn, as Modal shows it.
  useLayoutEffect(() => {
    const dialog = ref.current;
    if (!dialog || !enabled) return;
    let from: { x: number; y: number } | null = null;
    let began = 0;
    let offset = 0;
    const finish = (release: boolean) => {
      const far = offset > dialog.offsetHeight * sheetCloseShare;
      const fast = offset / (performance.now() - began) > sheetFlickSpeed;
      dialog.removeAttribute("data-dragging");
      dialog.style.removeProperty("--swipe");
      if (release && (far || fast)) latest.current();
    };
    const start = (event: TouchEvent) => {
      from = null;
      const target = event.target as Element;
      if (
        event.touches.length !== 1 ||
        !coarsePointer() ||
        target.closest('input, textarea, select, [role="slider"]')
      )
        return;
      // Anything under the finger scrolled away from its top scrolls, and
      // what takes its own touches (touch-action: none, as the canvas does)
      // keeps them; the sheet's own header takes them for this pull.
      for (let at: Element | null = target; at; at = at.parentElement) {
        if (at.scrollTop > 0) return;
        if (at === dialog) break;
        if (
          at.parentElement !== dialog &&
          getComputedStyle(at).touchAction === "none"
        )
          return;
      }
      const [touch] = event.touches;
      from = { x: touch.clientX, y: touch.clientY };
    };
    const move = (event: TouchEvent) => {
      if (!from) return;
      const [touch] = event.touches;
      const dx = touch.clientX - from.x;
      const dy = touch.clientY - from.y;
      if (!dialog.hasAttribute("data-dragging")) {
        if (Math.hypot(dx, dy) < swipeSlopPx) return;
        // Upward or sideways movement, or a scroll already under way, is
        // not a pull.
        if (dy <= 0 || Math.abs(dx) > dy || !event.cancelable) {
          from = null;
          return;
        }
        Object.assign(from, { x: touch.clientX, y: touch.clientY });
        began = performance.now();
        dialog.setAttribute("data-dragging", "");
        offset = 0;
        return event.preventDefault();
      }
      event.preventDefault();
      offset = Math.max(0, touch.clientY - from.y);
      dialog.style.setProperty("--swipe", `${offset}px`);
    };
    const end = (event: TouchEvent) => {
      if (from && dialog.hasAttribute("data-dragging"))
        finish(event.type === "touchend");
      from = null;
    };
    dialog.addEventListener("touchstart", start, { passive: true });
    dialog.addEventListener("touchmove", move, { passive: false });
    dialog.addEventListener("touchend", end, { passive: true });
    dialog.addEventListener("touchcancel", end, { passive: true });
    return () => {
      dialog.removeEventListener("touchstart", start);
      dialog.removeEventListener("touchmove", move);
      dialog.removeEventListener("touchend", end);
      dialog.removeEventListener("touchcancel", end);
    };
  }, [ref, enabled, latest]);
}

// A swipe from the left edge opens the conversations drawer. Text that scrolls
// sideways, the terminal and anything inside a dialog keep their touches.
export function useEdgeSwipe(enabled: boolean, open: () => void) {
  const latest = useLatest(open);
  useEffect(() => {
    if (!enabled) return;
    let from: { x: number; y: number } | null = null;
    const start = (event: TouchEvent) => {
      from = null;
      const [touch] = event.touches;
      const target = event.target as Element;
      if (
        event.touches.length !== 1 ||
        touch.clientX > viewportBounds(true).left + edgeSwipeStartPx ||
        document.querySelector("dialog[open]") ||
        target.closest(".terminal-view")
      )
        return;
      for (let at: Element | null = target; at; at = at.parentElement)
        if (
          at.scrollWidth > at.clientWidth &&
          /auto|scroll/.test(getComputedStyle(at).overflowX)
        )
          return;
      from = { x: touch.clientX, y: touch.clientY };
      // Safari starts its back swipe from here unless the touch is cancelled;
      // a control's touch has to stay a tap.
      if (
        event.cancelable &&
        !target.closest("a, button, input, label, select")
      )
        event.preventDefault();
    };
    const move = (event: TouchEvent) => {
      if (!from) return;
      const [touch] = event.touches;
      const dx = touch.clientX - from.x;
      const dy = Math.abs(touch.clientY - from.y);
      if (dy > dx) {
        if (dy > swipeSlopPx) from = null;
        return;
      }
      if (dx < edgeSwipeOpenPx) return;
      from = null;
      latest.current();
    };
    const stop = () => (from = null);
    document.addEventListener("touchstart", start, { passive: false });
    document.addEventListener("touchmove", move, { passive: true });
    document.addEventListener("touchend", stop, { passive: true });
    document.addEventListener("touchcancel", stop, { passive: true });
    return () => {
      document.removeEventListener("touchstart", start);
      document.removeEventListener("touchmove", move);
      document.removeEventListener("touchend", stop);
      document.removeEventListener("touchcancel", stop);
    };
  }, [enabled, latest]);
}
