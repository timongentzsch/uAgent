import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Minus, Plus, Scan } from "lucide-preact";
import { IconButton } from "./ui.tsx";
import {
  capturePointer,
  constrainView,
  MAXIMUM_ZOOM,
  panView,
  pinchStart,
  pinchTo,
  pinchView,
  type Pinch,
  type Point,
  type ZoomView,
  TAP_SLOP_PX,
} from "./zoom.ts";

const DOUBLE_TAP_MS = 300;
const DOUBLE_TAP_ZOOM = 2.5;
// One press of − or +, or of ⌘/Ctrl with − or +.
const ZOOM_STEP = 1.25;

// Media that zooms like a native viewer: pinch, double-tap or double-click
// to a point, ⌘/Ctrl+wheel or a trackpad pinch, the − and + controls or
// ⌘/Ctrl with −, + and 0, and drag or scroll to pan once zoomed. Fit shows
// the whole content. A tap on the empty surface around the content
// dismisses.
//
// `draw` hands single pointers to the content (a pen, a pin): two fingers
// still pinch and scroll still pans, but nothing pans on one pointer or
// dismisses. `natural` is the content's natural width, so the level reads
// as a percentage of its real size; `onView` follows every change.
export function ZoomSurface({
  label,
  dismiss,
  draw = false,
  natural,
  onView,
  children,
}: {
  label: string;
  dismiss?: () => void;
  draw?: boolean;
  natural?: number;
  onView?: (view: ZoomView) => void;
  children: ComponentChildren;
}) {
  const surface = useRef<HTMLDivElement>(null);
  const content = useRef<HTMLDivElement>(null);
  const view = useRef<ZoomView>({ scale: 1, x: 0, y: 0 });
  const [scale, setScale] = useState(1);
  // The level shown: against the content's real size, else against the fit.
  const [percent, setPercent] = useState(100);
  // Read at apply time: the resize and wheel handlers bind once.
  const naturalWidth = useRef(natural);
  naturalWidth.current = natural;
  const viewChanged = useRef(onView);
  viewChanged.current = onView;
  const points = useRef(new Map<number, Point>());
  const pinch = useRef<Pinch | null>(null);
  const start = useRef<Point | null>(null);
  const moved = useRef(false);
  // Where the gesture began, read before pointer capture retargets the rest
  // of it (pointerup, click, dblclick) to the surface itself.
  const onBackground = useRef(false);
  const mouse = useRef(false);
  const lastTap = useRef<{ time: number; at: Point } | null>(null);

  const bounds = () => surface.current!.getBoundingClientRect();
  const local = (point: Point) => {
    const rect = bounds();
    return { x: point.x - rect.left, y: point.y - rect.top };
  };
  // Gestures track the finger exactly; taps, controls and keys glide.
  const apply = (next: ZoomView, glide = false) => {
    const element = content.current;
    if (!surface.current || !element) return;
    element.toggleAttribute("data-glide", glide);
    const { width, height } = bounds();
    view.current = constrainView(next, width, height);
    const { scale, x, y } = view.current;
    element.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
    surface.current.toggleAttribute("data-zoomed", scale > 1);
    const base = (element.firstElementChild as HTMLElement | null)?.offsetWidth;
    const real = naturalWidth.current;
    setScale(scale);
    setPercent(Math.round(scale * (real && base ? base / real : 1) * 100));
    viewChanged.current?.(view.current);
  };
  const zoomAt = (point: Point, ratio: number, glide = false) => {
    const { width, height } = bounds();
    const at = local(point);
    apply(pinchView(view.current, width, height, at, at, ratio), glide);
  };
  // − and + zoom around the middle of what is on screen.
  const step = (ratio: number) => {
    const { left, top, width, height } = bounds();
    zoomAt({ x: left + width / 2, y: top + height / 2 }, ratio, true);
  };
  const fit = () => apply({ scale: 1, x: 0, y: 0 }, true);
  const toggle = (point: Point) =>
    zoomAt(point, view.current.scale > 1 ? 0 : DOUBLE_TAP_ZOOM, true);
  const background = (target: EventTarget | null) =>
    target === surface.current || target === content.current;

  useEffect(() => {
    const element = surface.current;
    if (!element) return;
    const observer = new ResizeObserver(() => apply(view.current));
    observer.observe(element);
    // ⌘ on a Mac, Ctrl elsewhere; a trackpad pinch arrives as Ctrl+wheel.
    const wheel = (event: WheelEvent) => {
      if (event.ctrlKey || event.metaKey) {
        event.preventDefault();
        zoomAt(
          { x: event.clientX, y: event.clientY },
          Math.exp(-event.deltaY * 0.01),
        );
      } else if (view.current.scale > 1) {
        event.preventDefault();
        const { width, height } = bounds();
        apply(
          panView(view.current, width, height, -event.deltaX, -event.deltaY),
        );
      }
    };
    // The browser's own page zoom keys zoom the content instead.
    const key = (event: KeyboardEvent) => {
      if (!(event.ctrlKey || event.metaKey) || event.altKey) return;
      if (event.key === "=" || event.key === "+") step(ZOOM_STEP);
      else if (event.key === "-") step(1 / ZOOM_STEP);
      else if (event.key === "0") fit();
      else return;
      event.preventDefault();
    };
    element.addEventListener("wheel", wheel, { passive: false });
    addEventListener("keydown", key);
    return () => {
      observer.disconnect();
      element.removeEventListener("wheel", wheel);
      removeEventListener("keydown", key);
    };
  }, []);

  useEffect(() => apply(view.current), [natural]);
  return (
    <div
      class="zoom-surface"
      ref={surface}
      role="group"
      aria-label={label}
      data-draw={draw || undefined}
      onPointerDown={(event) => {
        if (event.pointerType === "mouse" && event.button !== 0) return;
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        if (points.current.size === 2) {
          pinch.current = pinchStart(
            [...points.current.values()].map(local),
            view.current,
          );
          moved.current = true;
        }
        // A drawing keeps its own pointer; only a pinch belongs here.
        if (draw) return;
        if (points.current.size === 1) {
          onBackground.current = background(event.target);
          mouse.current = event.pointerType === "mouse";
          start.current = point;
          moved.current = false;
        }
        capturePointer(event.currentTarget, event.pointerId);
      }}
      onPointerMove={(event) => {
        const previous = points.current.get(event.pointerId);
        if (!previous) return;
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        const { width, height } = bounds();
        if (points.current.size === 2 && pinch.current) {
          const inside = [...points.current.values()].map(local);
          apply(pinchTo(pinch.current, inside, width, height));
          return;
        }
        if (draw) return;
        if (
          start.current &&
          Math.hypot(point.x - start.current.x, point.y - start.current.y) >
            TAP_SLOP_PX
        )
          moved.current = true;
        if (moved.current && view.current.scale > 1)
          apply(
            panView(
              view.current,
              width,
              height,
              point.x - previous.x,
              point.y - previous.y,
            ),
          );
      }}
      onPointerUp={(event) => {
        if (!points.current.delete(event.pointerId)) return;
        if (points.current.size < 2) pinch.current = null;
        if (draw || points.current.size || moved.current) return;
        const at = { x: event.clientX, y: event.clientY };
        if (onBackground.current) {
          dismiss?.();
          return;
        }
        // Touch double-taps here; a mouse uses the native double-click.
        if (event.pointerType === "mouse") return;
        const tap = lastTap.current;
        if (
          tap &&
          event.timeStamp - tap.time < DOUBLE_TAP_MS &&
          Math.hypot(at.x - tap.at.x, at.y - tap.at.y) < 4 * TAP_SLOP_PX
        ) {
          lastTap.current = null;
          toggle(at);
        } else lastTap.current = { time: event.timeStamp, at };
      }}
      onPointerCancel={(event) => {
        points.current.delete(event.pointerId);
        pinch.current = null;
      }}
      onDblClick={(event) => {
        // Touch double-taps are handled above; browsers may add a dblclick.
        if (!draw && mouse.current && !onBackground.current)
          toggle({ x: event.clientX, y: event.clientY });
      }}
    >
      <div class="zoom-content" ref={content}>
        {children}
      </div>
      <div
        class="zoom-controls"
        role="group"
        aria-label="Zoom"
        // Presses here are the controls', not the surface's gestures.
        onPointerDown={(event) => event.stopPropagation()}
        onDblClick={(event) => event.stopPropagation()}
      >
        <IconButton
          label="Zoom out"
          disabled={scale <= 1}
          onClick={() => step(1 / ZOOM_STEP)}
        >
          <Minus />
        </IconButton>
        <output aria-live="polite">{percent}%</output>
        <IconButton
          label="Zoom in"
          disabled={scale >= MAXIMUM_ZOOM}
          onClick={() => step(ZOOM_STEP)}
        >
          <Plus />
        </IconButton>
        <IconButton label="Fit" disabled={scale <= 1} onClick={fit}>
          <Scan />
        </IconButton>
      </div>
    </div>
  );
}
