import type { ComponentChildren } from "preact";
import { useEffect, useRef } from "preact/hooks";
import {
  capturePointer,
  constrainView,
  distance,
  midpoint,
  panView,
  pinchView,
  type Point,
  type ZoomView,
} from "./zoom.ts";

const SLOP_PX = 8;
const DOUBLE_TAP_MS = 300;
const DOUBLE_TAP_ZOOM = 2.5;

// Media that zooms like a native viewer: pinch, double-tap or double-click
// to a point, Ctrl+wheel (a trackpad pinch) on desktop, and drag or scroll to
// pan once zoomed. A tap on the empty surface around the content dismisses.
export function ZoomSurface({
  label,
  dismiss,
  children,
}: {
  label: string;
  dismiss: () => void;
  children: ComponentChildren;
}) {
  const surface = useRef<HTMLDivElement>(null);
  const content = useRef<HTMLDivElement>(null);
  const view = useRef<ZoomView>({ scale: 1, x: 0, y: 0 });
  const points = useRef(new Map<number, Point>());
  const pinch = useRef<{
    distance: number;
    midpoint: Point;
    view: ZoomView;
  } | null>(null);
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
  // Gestures track the finger exactly; a double-tap or double-click glides.
  const apply = (next: ZoomView, glide = false) => {
    const element = content.current;
    if (!surface.current || !element) return;
    element.toggleAttribute("data-glide", glide);
    const { width, height } = bounds();
    view.current = constrainView(next, width, height);
    const { scale, x, y } = view.current;
    element.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
    surface.current.toggleAttribute("data-zoomed", scale > 1);
  };
  const zoomAt = (point: Point, ratio: number, glide = false) => {
    const { width, height } = bounds();
    const at = local(point);
    apply(pinchView(view.current, width, height, at, at, ratio), glide);
  };
  const toggle = (point: Point) =>
    zoomAt(point, view.current.scale > 1 ? 0 : DOUBLE_TAP_ZOOM, true);
  const background = (target: EventTarget | null) =>
    target === surface.current || target === content.current;

  useEffect(() => {
    const element = surface.current;
    if (!element) return;
    const observer = new ResizeObserver(() => apply(view.current));
    observer.observe(element);
    const wheel = (event: WheelEvent) => {
      if (event.ctrlKey) {
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
    element.addEventListener("wheel", wheel, { passive: false });
    return () => {
      observer.disconnect();
      element.removeEventListener("wheel", wheel);
    };
  }, []);

  return (
    <div
      class="zoom-surface"
      ref={surface}
      role="group"
      aria-label={label}
      onPointerDown={(event) => {
        if (event.pointerType === "mouse" && event.button !== 0) return;
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        if (points.current.size === 1) {
          onBackground.current = background(event.target);
          mouse.current = event.pointerType === "mouse";
          start.current = point;
          moved.current = false;
        }
        capturePointer(event.currentTarget, event.pointerId);
        if (points.current.size === 2) {
          const inside = [...points.current.values()].map(local);
          pinch.current = {
            distance: distance(inside),
            midpoint: midpoint(inside),
            view: { ...view.current },
          };
          moved.current = true;
        }
      }}
      onPointerMove={(event) => {
        const previous = points.current.get(event.pointerId);
        if (!previous) return;
        const point = { x: event.clientX, y: event.clientY };
        points.current.set(event.pointerId, point);
        const { width, height } = bounds();
        if (points.current.size === 2 && pinch.current) {
          const inside = [...points.current.values()].map(local);
          const from = pinch.current;
          apply(
            pinchView(
              from.view,
              width,
              height,
              from.midpoint,
              midpoint(inside),
              from.distance > 0 ? distance(inside) / from.distance : 1,
            ),
          );
          return;
        }
        if (
          start.current &&
          Math.hypot(point.x - start.current.x, point.y - start.current.y) >
            SLOP_PX
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
        if (points.current.size || moved.current) return;
        const at = { x: event.clientX, y: event.clientY };
        if (onBackground.current) {
          dismiss();
          return;
        }
        // Touch double-taps here; a mouse uses the native double-click.
        if (event.pointerType === "mouse") return;
        const tap = lastTap.current;
        if (
          tap &&
          event.timeStamp - tap.time < DOUBLE_TAP_MS &&
          Math.hypot(at.x - tap.at.x, at.y - tap.at.y) < 4 * SLOP_PX
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
        if (mouse.current && !onBackground.current)
          toggle({ x: event.clientX, y: event.clientY });
      }}
    >
      <div class="zoom-content" ref={content}>
        {children}
      </div>
    </div>
  );
}
