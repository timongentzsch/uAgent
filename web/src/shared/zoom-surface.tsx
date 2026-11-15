import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Minus, Plus, Scan } from "lucide-preact";
import { IconButton } from "./ui.tsx";
import {
  bindZoomWheel,
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
import "./zoom-surface.css";
import { useLatest } from "./use-latest.ts";

const DOUBLE_TAP_MS = 300;
const DOUBLE_TAP_ZOOM = 2.5;
// One press of − or +, or of ⌘/Ctrl with − or +.
const ZOOM_STEP = 1.25;
const FITTED: ZoomView = { scale: 1, x: 0, y: 0 };

// `draw` hands single pointers to the content: two fingers still pinch, but
// nothing pans on one pointer or dismisses. With `range` the content is a
// plane with no edges: one pointer pans at any scale and Fit shows `fit`.
export function ZoomSurface({
  label,
  dismiss,
  draw = false,
  natural,
  onView,
  range,
  fit: fitted,
  fitKey,
  over,
  children,
}: {
  label: string;
  dismiss?: () => void;
  draw?: boolean;
  natural?: number;
  onView?: (view: ZoomView) => void;
  range?: [number, number];
  fit?: (width: number, height: number) => ZoomView | undefined;
  fitKey?: unknown;
  over?: ComponentChildren;
  children: ComponentChildren;
}) {
  const free = !!range;
  const [least, most] = range ?? [1, MAXIMUM_ZOOM];
  const surface = useRef<HTMLDivElement>(null);
  const content = useRef<HTMLDivElement>(null);
  const view = useRef<ZoomView>(FITTED);
  const [scale, setScale] = useState(1);
  // The level shown: against the content's real size, else against the fit.
  const [percent, setPercent] = useState(100);
  // Read at apply time: the resize and wheel handlers bind once.
  const naturalWidth = useLatest(natural);
  const viewChanged = useLatest(onView);
  const fitView = useLatest(fitted);
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
    view.current = free ? next : constrainView(next, width, height);
    const { scale, x, y } = view.current;
    element.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
    surface.current.toggleAttribute("data-zoomed", free || scale > 1);
    const base = (element.firstElementChild as HTMLElement | null)?.offsetWidth;
    const real = naturalWidth.current;
    setScale(scale);
    setPercent(Math.round(scale * (real && base ? base / real : 1) * 100));
    viewChanged.current?.(view.current);
  };
  const pan = (dx: number, dy: number) => {
    const { width, height } = bounds();
    const { scale, x, y } = view.current;
    apply(
      free
        ? { scale, x: x + dx, y: y + dy }
        : panView(view.current, width, height, dx, dy),
    );
  };
  const zoomAt = (point: Point, ratio: number, glide = false) => {
    const { width, height } = bounds();
    const at = local(point);
    apply(pinchView(view.current, width, height, at, at, ratio, range), glide);
  };
  const step = (ratio: number) => {
    const { left, top, width, height } = bounds();
    zoomAt({ x: left + width / 2, y: top + height / 2 }, ratio, true);
  };
  const fit = (glide = true) => {
    const { width, height } = bounds();
    const home = free ? fitView.current?.(width, height) : FITTED;
    if (home) apply(home, glide);
  };
  const toggle = (point: Point) =>
    zoomAt(point, view.current.scale > 1 ? 0 : DOUBLE_TAP_ZOOM, true);
  const background = (target: EventTarget | null) =>
    target === surface.current || target === content.current;

  // biome-ignore lint/correctness/useExhaustiveDependencies: binds once; the helpers it calls read only refs and state setters, so the first render's are current
  useEffect(() => {
    const element = surface.current;
    if (!element) return;
    const observer = new ResizeObserver(() => apply(view.current));
    observer.observe(element);
    const unbindWheel = bindZoomWheel(
      element,
      (ratio, at) => {
        const { width, height } = bounds();
        apply(pinchView(view.current, width, height, at, at, ratio, range));
      },
      (dx, dy) => {
        if (!free && view.current.scale <= 1) return false;
        pan(dx, dy);
        return true;
      },
    );
    // Safari sends a trackpad pinch as gesture events, not the wheel. A
    // touch pinch sends them too, which the pointers already zoom.
    let pinched = 1;
    const gesture = (event: Event) => {
      const { scale, clientX, clientY } = event as Event & {
        scale: number;
        clientX: number;
        clientY: number;
      };
      event.preventDefault();
      if (event.type === "gesturestart") pinched = 1;
      else if (points.current.size < 2)
        zoomAt({ x: clientX, y: clientY }, scale / pinched);
      pinched = scale;
    };
    for (const kind of ["gesturestart", "gesturechange"])
      element.addEventListener(kind, gesture);
    // The browser's own page zoom keys zoom the content instead: always in
    // a dialog, which owns the keyboard, elsewhere while the focus is on the
    // surface. A plane takes none.
    const key = (event: KeyboardEvent) => {
      if (free || !(event.ctrlKey || event.metaKey) || event.altKey) return;
      if (
        !element.closest("dialog") &&
        !element.contains(document.activeElement)
      )
        return;
      if (event.key === "=" || event.key === "+") step(ZOOM_STEP);
      else if (event.key === "-") step(1 / ZOOM_STEP);
      else if (event.key === "0") fit();
      else return;
      event.preventDefault();
    };
    addEventListener("keydown", key);
    return () => {
      observer.disconnect();
      unbindWheel();
      for (const kind of ["gesturestart", "gesturechange"])
        element.removeEventListener(kind, gesture);
      removeEventListener("keydown", key);
    };
  }, []);

  // biome-ignore lint/correctness/useExhaustiveDependencies: re-applies when the content's real size arrives
  useEffect(() => apply(view.current), [natural]);
  // biome-ignore lint/correctness/useExhaustiveDependencies: a plane shows its fit when the caller says its content changed
  useEffect(() => {
    if (free) fit(false);
  }, [fitKey]);
  return (
    <div
      class="zoom-surface"
      ref={surface}
      role="group"
      aria-label={label}
      data-draw={draw || undefined}
      data-free={free || undefined}
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
        // A plane waits for the press to move: capturing now would take the
        // click from what is under it.
        if (!free) capturePointer(event.currentTarget, event.pointerId);
      }}
      onPointerMove={(event) => {
        const previous = points.current.get(event.pointerId);
        if (!previous) return;
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        const { width, height } = bounds();
        if (points.current.size === 2 && pinch.current) {
          const inside = [...points.current.values()].map(local);
          apply(pinchTo(pinch.current, inside, width, height, range));
          return;
        }
        if (draw) return;
        if (
          start.current &&
          Math.hypot(point.x - start.current.x, point.y - start.current.y) >
            TAP_SLOP_PX
        )
          moved.current = true;
        if (!moved.current) return;
        if (free) capturePointer(event.currentTarget, event.pointerId);
        if (free || view.current.scale > 1)
          pan(point.x - previous.x, point.y - previous.y);
      }}
      onPointerUp={(event) => {
        if (!points.current.delete(event.pointerId)) return;
        if (points.current.size < 2) pinch.current = null;
        if (draw || free || points.current.size || moved.current) return;
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
      // A press that moved the plane opens nothing under it; a new press,
      // on the controls too, starts over.
      onPointerDownCapture={() => {
        if (!points.current.size) moved.current = false;
      }}
      onClickCapture={(event) => {
        if (free && moved.current) event.stopPropagation();
      }}
      onDblClick={(event) => {
        // Touch double-taps are handled above; browsers may add a dblclick.
        if (!draw && !free && mouse.current && !onBackground.current)
          toggle({ x: event.clientX, y: event.clientY });
      }}
    >
      <div class="zoom-content" ref={content}>
        {children}
      </div>
      {over}
      <ZoomControls
        percent={percent}
        out={scale > least ? () => step(1 / ZOOM_STEP) : undefined}
        in={scale < most ? () => step(ZOOM_STEP) : undefined}
        fit={free || scale > 1 ? () => fit() : undefined}
      />
    </div>
  );
}

// Presses here are the controls', never the surface's gestures.
export function ZoomControls({
  percent,
  out,
  in: zoomIn,
  fit,
}: {
  percent: number;
  out?: () => void;
  in?: () => void;
  fit?: () => void;
}) {
  return (
    <div
      class="float-toolbar zoom-controls"
      role="group"
      aria-label="Zoom"
      onPointerDown={(event) => event.stopPropagation()}
      onDblClick={(event) => event.stopPropagation()}
    >
      <IconButton label="Zoom out" disabled={!out} onClick={out}>
        <Minus />
      </IconButton>
      <output aria-live="polite">{percent}%</output>
      <IconButton label="Zoom in" disabled={!zoomIn} onClick={zoomIn}>
        <Plus />
      </IconButton>
      <IconButton label="Fit" disabled={!fit} onClick={fit}>
        <Scan />
      </IconButton>
    </div>
  );
}
