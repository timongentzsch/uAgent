import type { ComponentChildren, RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import {
  capturePointer,
  constrainView,
  distance,
  followPointer,
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
  reveal,
  pointAt,
  trackpad,
  children,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
  readOnly: boolean;
  refreshPointer: () => void;
  reveal: RefObject<(point: Point) => void>;
  pointAt: (x: number, y: number) => void;
  trackpad: boolean;
  children: ComponentChildren;
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
    // Let noVNC scale its own canvas from the real container dimensions.
    // An outer CSS scale bypasses its pointer-coordinate conversion.
    element.style.width = `${scale * 100}%`;
    element.style.height = `${scale * 100}%`;
    element.style.transform = `translate(${x}px, ${y}px)`;
    refresh.current();
  };

  reveal.current = (point) => {
    const rect = screen.current?.getBoundingClientRect();
    if (!rect) return;
    setView(
      followPointer(view.current, rect.width, rect.height, {
        x: point.x - rect.left,
        y: point.y - rect.top,
      }),
    );
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
    // noVNC resizes its canvas after the container changes.
    if (target.current) observer.observe(target.current);
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
      data-trackpad={trackpad || undefined}
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
        if (event.pointerType === "mouse") {
          pointAt(event.clientX, event.clientY);
          return;
        }
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
      {children}
    </div>
  );
}

export default BrowserViewport;
