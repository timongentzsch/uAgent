import type { RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import {
  capturePointer,
  constrainView,
  distance,
  midpoint,
  panView,
  pinchView,
  type BrowserView,
  type Point,
  type TrackedPoint,
} from "./gestures.ts";

function BrowserViewport({
  screen,
  target,
  readOnly,
  refreshPointer,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
  readOnly: boolean;
  refreshPointer: () => void;
}) {
  const pointers = useRef(new Map<number, TrackedPoint>());
  const view = useRef<BrowserView>({ scale: 1, x: 0, y: 0 });
  const refresh = useRef(refreshPointer);
  refresh.current = refreshPointer;
  const pinch = useRef<{
    distance: number;
    midpoint: Point;
    view: BrowserView;
  } | null>(null);

  const setView = (next: BrowserView) => {
    const rect = screen.current?.getBoundingClientRect();
    const element = target.current;
    if (!rect || !element) return;
    view.current = constrainView(next, rect.width, rect.height);
    const { scale, x, y } = view.current;
    element.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
    refresh.current();
  };

  const resetRemainingPointer = () => {
    pinch.current = null;
    const point = [...pointers.current.values()][0];
    if (!point) return;
    point.startX = point.x;
    point.startY = point.y;
    point.time = performance.now();
  };

  useEffect(() => {
    const element = screen.current;
    if (!element) return;
    const reset = () => {
      pointers.current.clear();
      pinch.current = null;
    };
    const hide = () => {
      if (document.visibilityState === "hidden") reset();
    };
    const observer = new ResizeObserver(() => setView(view.current));
    const blockNoVncTouch = (event: Event) => {
      event.preventDefault();
      event.stopPropagation();
    };
    observer.observe(element);
    for (const kind of ["touchstart", "touchmove", "touchend", "touchcancel"])
      element.addEventListener(kind, blockNoVncTouch, {
        capture: true,
        passive: false,
      });
    addEventListener("blur", reset);
    document.addEventListener("visibilitychange", hide);
    return () => {
      observer.disconnect();
      for (const kind of ["touchstart", "touchmove", "touchend", "touchcancel"])
        element.removeEventListener(kind, blockNoVncTouch, true);
      removeEventListener("blur", reset);
      document.removeEventListener("visibilitychange", hide);
    };
  }, []);

  return (
    <div
      class={`browser-screen${readOnly ? " readonly" : ""}`}
      ref={screen}
      role="group"
      aria-label="Browser viewport"
      onPointerDownCapture={(event) => {
        if (event.pointerType === "mouse") return;
        event.preventDefault();
        capturePointer(event.currentTarget, event.pointerId);
        const rect = event.currentTarget.getBoundingClientRect();
        const point = {
          x: event.clientX - rect.left,
          y: event.clientY - rect.top,
          startX: event.clientX - rect.left,
          startY: event.clientY - rect.top,
          time: event.timeStamp,
        };
        pointers.current.set(event.pointerId, point);
        if (pointers.current.size === 2) {
          const points = [...pointers.current.values()];
          pinch.current = {
            distance: distance(points),
            midpoint: midpoint(points),
            view: { ...view.current },
          };
        }
      }}
      onPointerMoveCapture={(event) => {
        const point = pointers.current.get(event.pointerId);
        const rect = screen.current?.getBoundingClientRect();
        if (!point || !rect) return;
        event.preventDefault();
        const x = event.clientX - rect.left;
        const y = event.clientY - rect.top;
        const dx = x - point.x;
        const dy = y - point.y;
        point.x = x;
        point.y = y;
        if (pointers.current.size === 1) {
          if (view.current.scale > 1)
            setView(panView(view.current, rect.width, rect.height, dx, dy));
          return;
        }
        if (pointers.current.size !== 2 || !pinch.current) return;
        const points = [...pointers.current.values()];
        const ratio =
          pinch.current.distance > 0
            ? distance(points) / pinch.current.distance
            : 1;
        setView(
          pinchView(
            pinch.current.view,
            rect.width,
            rect.height,
            pinch.current.midpoint,
            midpoint(points),
            ratio,
          ),
        );
      }}
      onPointerUpCapture={(event) => {
        pointers.current.delete(event.pointerId);
        resetRemainingPointer();
      }}
      onPointerCancelCapture={(event) => {
        pointers.current.delete(event.pointerId);
        resetRemainingPointer();
      }}
    >
      <div
        class="browser-rfb"
        ref={target}
        aria-label={
          readOnly ? "Read-only browser display" : "Interactive browser display"
        }
      />
    </div>
  );
}

export default BrowserViewport;
