import type { ComponentChildren, RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import { ArrowDown, ArrowUp, Minus, Plus } from "lucide-preact";
import { BROWSER_GESTURE, WHEEL, wheelNotches } from "./gestures.ts";
import { IconButton } from "../../shared/ui.tsx";
import { motionEase, motionMs } from "../../shared/motion.ts";
import {
  capturePointer,
  constrainView,
  distance,
  midpoint,
  panView,
  pinchStart,
  pinchTo,
  pinchView,
  type Pinch,
  type Point,
  type ZoomView,
} from "../../shared/zoom.ts";

type Gesture =
  | { kind: "pending"; start: Point }
  | { kind: "scroll"; travel: Point; at: Point }
  | { kind: "pan" }
  | { kind: "drag" }
  // Two fingers: a tap right-clicks; once they move, they pinch and pan.
  | ({ kind: "two"; at: Point; time: number; moved: boolean } & Pinch)
  | { kind: "done" };

// The remote screen under direct touch. `pointer` receives framebuffer
// pixels and RFB button masks; without it (watching) touch only zooms and
// pans this device's view. A mouse goes straight to noVNC.
export default function BrowserTouch({
  screen,
  target,
  pointer,
  label,
  children,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
  pointer?: (x: number, y: number, mask: number) => void;
  label: string;
  children?: ComponentChildren;
}) {
  const ring = useRef<HTMLSpanElement>(null);
  const points = useRef(new Map<number, Point>());
  const gesture = useRef<Gesture | null>(null);
  const hold = useRef<number | undefined>(undefined);
  const view = useRef<ZoomView>({ scale: 1, x: 0, y: 0 });
  const send = useRef(pointer);
  send.current = pointer;
  // The screen's size, kept by the resize observer: a moving finger reads
  // no layout for it.
  const size = useRef({ width: 0, height: 0 });

  const setView = (next: ZoomView) => {
    const element = target.current;
    if (!element) return;
    const { width, height } = size.current;
    view.current = constrainView(next, width, height);
    const { scale, x, y } = view.current;
    // noVNC scales its canvas from the container's real size; an outer CSS
    // scale would bypass its coordinate conversion.
    element.style.width = `${scale * 100}%`;
    element.style.height = `${scale * 100}%`;
    element.style.transform = `translate(${x}px, ${y}px)`;
  };
  // A client point as a framebuffer pixel. Off the remote screen it is null,
  // or with `clamp` the nearest edge pixel, so a held drag always lets go.
  const remote = (point: Point, clamp = false) => {
    const canvas = target.current?.querySelector("canvas");
    const rect = canvas?.getBoundingClientRect();
    if (!canvas || !rect?.width || !rect.height) return null;
    const x = ((point.x - rect.left) / rect.width) * canvas.width;
    const y = ((point.y - rect.top) / rect.height) * canvas.height;
    const inside = x >= 0 && y >= 0 && x < canvas.width && y < canvas.height;
    if (!inside && !clamp) return null;
    return {
      x: Math.max(0, Math.min(canvas.width - 1, x)),
      y: Math.max(0, Math.min(canvas.height - 1, y)),
    };
  };
  const deliver = (point: Point, clamp: boolean, masks: number[]) => {
    const at = remote(point, clamp);
    if (at && send.current)
      for (const mask of masks) send.current(at.x, at.y, mask);
  };
  const emit = (point: Point, ...masks: number[]) =>
    deliver(point, false, masks);
  // A held button follows the finger past the edge and always lets go.
  const emitHeld = (point: Point, ...masks: number[]) =>
    deliver(point, true, masks);
  // One reused ring shows where a touch landed; it stays while a hold drags.
  const feedback = (point: Point, held: boolean) => {
    const element = ring.current;
    const rect = screen.current?.getBoundingClientRect();
    if (!element || !rect) return;
    element.style.left = `${point.x - rect.left}px`;
    element.style.top = `${point.y - rect.top}px`;
    // The hold ring fills over the hold time itself: it is feedback on the
    // gesture, not decoration, so it keeps its timing under reduced motion.
    element.animate(
      held
        ? [
            { opacity: 0.35, transform: "scale(0.6)" },
            { opacity: 0.6, transform: "scale(1)" },
          ]
        : [
            { opacity: 0.6, transform: "scale(0.6)" },
            { opacity: 0, transform: "scale(1)" },
          ],
      {
        duration: held ? BROWSER_GESTURE.holdMs : motionMs("slow"),
        easing: motionEase("out"),
        fill: "forwards",
      },
    );
  };
  const fade = () =>
    ring.current?.animate([{ opacity: 0 }], {
      duration: motionMs("fast"),
      fill: "forwards",
    });
  const cancelHold = () => {
    clearTimeout(hold.current);
    hold.current = undefined;
  };
  // Buttons for what a pinch and a swipe do, for anyone who cannot make
  // them: zoom this device's view about its centre, scroll the page.
  const zoomBy = (ratio: number) => {
    const { width, height } = size.current;
    const centre = { x: width / 2, y: height / 2 };
    setView(pinchView(view.current, width, height, centre, centre, ratio));
  };
  const scrollBy = (mask: number) => {
    // The middle of the remote screen, wherever it is letterboxed.
    const rect = target.current
      ?.querySelector("canvas")
      ?.getBoundingClientRect();
    if (!rect) return;
    const centre = {
      x: rect.left + rect.width / 2,
      y: rect.top + rect.height / 2,
    };
    for (let notch = 0; notch < 3; notch++) emit(centre, mask, 0);
  };
  const current = () => [...points.current.values()];
  const local = (point: Point) => {
    const rect = screen.current!.getBoundingClientRect();
    return { x: point.x - rect.left, y: point.y - rect.top };
  };
  const reset = () => {
    cancelHold();
    if (gesture.current?.kind === "drag") {
      const [last] = current();
      if (last) emitHeld(last, 0);
      fade();
    }
    points.current.clear();
    gesture.current = null;
  };

  useEffect(() => {
    const element = screen.current;
    if (!element) return;
    const observer = new ResizeObserver(() => {
      size.current = element.getBoundingClientRect();
      setView(view.current);
    });
    observer.observe(element);
    // noVNC listens for touch itself; direct touch is handled here.
    const block = (event: Event) => {
      // Controls laid over the screen keep their native touch behaviour.
      if (
        (event.target as Element).closest(
          ".browser-card, .browser-view-controls",
        )
      )
        return;
      event.preventDefault();
      event.stopPropagation();
    };
    const kinds = ["touchstart", "touchmove", "touchend", "touchcancel"];
    for (const kind of kinds)
      element.addEventListener(kind, block, { capture: true, passive: false });
    const hidden = () => document.visibilityState === "hidden" && reset();
    addEventListener("blur", reset);
    document.addEventListener("visibilitychange", hidden);
    return () => {
      observer.disconnect();
      for (const kind of kinds) element.removeEventListener(kind, block, true);
      removeEventListener("blur", reset);
      document.removeEventListener("visibilitychange", hidden);
      cancelHold();
    };
  }, []);

  return (
    <div
      class="browser-screen"
      ref={screen}
      role="group"
      aria-label={label}
      onPointerDownCapture={(event) => {
        // Mice go to noVNC; controls laid over the screen keep their taps.
        if (
          event.pointerType === "mouse" ||
          (event.target as Element).closest(
            ".browser-card, .browser-view-controls",
          )
        )
          return;
        event.preventDefault();
        capturePointer(event.currentTarget, event.pointerId);
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        const all = current();
        if (all.length === 1) {
          gesture.current = { kind: "pending", start: point };
          if (send.current)
            hold.current = window.setTimeout(() => {
              if (gesture.current?.kind !== "pending") return;
              gesture.current = { kind: "drag" };
              emit(point, 1);
              feedback(point, true);
            }, BROWSER_GESTURE.holdMs);
        } else if (all.length === 2) {
          cancelHold();
          if (gesture.current?.kind === "drag") {
            emitHeld(all[0], 0);
            fade();
          }
          gesture.current = {
            kind: "two",
            at: midpoint(all),
            time: event.timeStamp,
            moved: false,
            ...pinchStart(all.map(local), view.current),
          };
        }
      }}
      onPointerMoveCapture={(event) => {
        const previous = points.current.get(event.pointerId);
        if (!previous) return;
        event.preventDefault();
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        const state = gesture.current;
        const moved = (from: Point) =>
          Math.hypot(point.x - from.x, point.y - from.y) >
          BROWSER_GESTURE.movementSlopPx;
        if (state?.kind === "pending" && moved(state.start)) {
          cancelHold();
          gesture.current =
            send.current && view.current.scale === 1
              ? { kind: "scroll", travel: { x: 0, y: 0 }, at: state.start }
              : { kind: "pan" };
        }
        const next = gesture.current;
        if (next?.kind === "scroll") {
          const { masks, rest } = wheelNotches({
            x: next.travel.x + point.x - previous.x,
            y: next.travel.y + point.y - previous.y,
          });
          next.travel = rest;
          for (const mask of masks) emit(next.at, mask, 0);
        } else if (next?.kind === "pan") {
          setView(
            panView(
              view.current,
              size.current.width,
              size.current.height,
              point.x - previous.x,
              point.y - previous.y,
            ),
          );
        } else if (next?.kind === "drag") {
          emitHeld(point, 1);
          feedback(point, true);
        } else if (next?.kind === "two" && points.current.size === 2) {
          const all = current().map(local);
          next.moved ||=
            Math.abs(distance(all) - next.distance) >
              BROWSER_GESTURE.movementSlopPx ||
            Math.hypot(
              midpoint(all).x - next.midpoint.x,
              midpoint(all).y - next.midpoint.y,
            ) > BROWSER_GESTURE.movementSlopPx;
          if (!next.moved) return;
          setView(pinchTo(next, all, size.current.width, size.current.height));
        }
      }}
      onPointerUpCapture={(event) => {
        const point = points.current.get(event.pointerId);
        if (!point) return;
        const state = gesture.current;
        cancelHold();
        if (state?.kind === "pending" && send.current) {
          emit(point, 0, 1, 0);
          feedback(point, false);
        } else if (state?.kind === "drag") {
          emitHeld(point, 0);
          fade();
        } else if (
          state?.kind === "two" &&
          !state.moved &&
          event.timeStamp - state.time < BROWSER_GESTURE.tapMs &&
          send.current
        ) {
          emit(state.at, 0, 4, 0);
          feedback(state.at, false);
        }
        points.current.delete(event.pointerId);
        // The rest of a multi-finger gesture ends with its last finger.
        gesture.current = points.current.size ? { kind: "done" } : null;
      }}
      onPointerCancelCapture={reset}
    >
      <div class="browser-rfb" ref={target} />
      <span class="browser-touch" ref={ring} aria-hidden="true" />
      <div class="browser-view-controls" role="group" aria-label="View">
        <IconButton label="Zoom out" onClick={() => zoomBy(1 / 1.25)}>
          <Minus />
        </IconButton>
        <IconButton label="Zoom in" onClick={() => zoomBy(1.25)}>
          <Plus />
        </IconButton>
        {pointer && (
          <>
            <IconButton label="Scroll up" onClick={() => scrollBy(WHEEL.up)}>
              <ArrowUp />
            </IconButton>
            <IconButton
              label="Scroll down"
              onClick={() => scrollBy(WHEEL.down)}
            >
              <ArrowDown />
            </IconButton>
          </>
        )}
      </div>
      {children}
    </div>
  );
}
