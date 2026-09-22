import type RFB from "@novnc/novnc";
import type { RefObject } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";

const MAX_SCALE = 5;
const MOVE_THRESHOLD = 6;
const LONG_PRESS_MS = 500;
const TAP_MS = 350;
const TRACKPAD_SPEED = 1.4;

interface Point {
  x: number;
  y: number;
  startX: number;
  startY: number;
}

interface View {
  scale: number;
  x: number;
  y: number;
}

interface Gesture {
  started: number;
  moved: boolean;
  two: boolean;
  startDistance: number;
  startMidX: number;
  startMidY: number;
  lastMidY: number;
  view: View;
}

const distance = (points: Point[]) =>
  Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y);

const midpoint = (points: Point[]) => ({
  x: (points[0].x + points[1].x) / 2,
  y: (points[0].y + points[1].y) / 2,
});

export default function BrowserTouch({
  screen,
  target,
  rfb,
  disabled,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
  rfb: RefObject<RFB>;
  disabled: boolean;
}) {
  const pointers = useRef(new Map<number, Point>());
  const gesture = useRef<Gesture | null>(null);
  const cursor = useRef({ x: 0.5, y: 0.5 });
  const view = useRef<View>({ scale: 1, x: 0, y: 0 });
  const hold = useRef<number | null>(null);
  const holdTimer = useRef<number | null>(null);
  const [cursorStyle, setCursorStyle] = useState({ left: "50%", top: "50%" });

  const clearHold = () => {
    if (holdTimer.current !== null) clearTimeout(holdTimer.current);
    holdTimer.current = null;
  };

  const bounds = () => screen.current?.getBoundingClientRect();
  const canvas = () => target.current?.querySelector("canvas");
  const cursorPoint = () => {
    const rect = bounds();
    if (!rect) return null;
    return {
      x: view.current.x + cursor.current.x * rect.width * view.current.scale,
      y: view.current.y + cursor.current.y * rect.height * view.current.scale,
      rect,
    };
  };

  const drawCursor = () => {
    const point = cursorPoint();
    if (!point) return;
    setCursorStyle({ left: `${point.x}px`, top: `${point.y}px` });
  };

  const mouse = (
    kind: "mousedown" | "mouseup" | "mousemove",
    button = 0,
    buttons = 0,
  ) => {
    const element = canvas();
    const point = cursorPoint();
    if (!element || !point || disabled) return;
    element.dispatchEvent(
      new MouseEvent(kind, {
        bubbles: true,
        cancelable: true,
        clientX: point.rect.left + point.x,
        clientY: point.rect.top + point.y,
        button,
        buttons,
      }),
    );
  };

  const setView = (next: View) => {
    const rect = bounds();
    const element = target.current;
    if (!rect || !element) return;
    const scale = Math.max(1, Math.min(MAX_SCALE, next.scale));
    const x = Math.max(rect.width * (1 - scale), Math.min(0, next.x));
    const y = Math.max(rect.height * (1 - scale), Math.min(0, next.y));
    view.current = { scale, x, y };
    element.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
    drawCursor();
  };

  const moveCursor = (dx: number, dy: number) => {
    const rect = bounds();
    if (!rect) return;
    const current = view.current;
    const width = rect.width * current.scale;
    const height = rect.height * current.scale;
    const minX = Math.max(0, -current.x / width);
    const maxX = Math.min(1, (rect.width - current.x) / width);
    const minY = Math.max(0, -current.y / height);
    const maxY = Math.min(1, (rect.height - current.y) / height);
    cursor.current.x = Math.max(
      minX,
      Math.min(maxX, cursor.current.x + (dx * TRACKPAD_SPEED) / width),
    );
    cursor.current.y = Math.max(
      minY,
      Math.min(maxY, cursor.current.y + (dy * TRACKPAD_SPEED) / height),
    );
    drawCursor();
    mouse("mousemove", 0, hold.current === null ? 0 : 1);
  };

  const wheel = (dy: number) => {
    const element = canvas();
    const point = cursorPoint();
    if (!element || !point || disabled) return;
    element.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: point.rect.left + point.x,
        clientY: point.rect.top + point.y,
        deltaY: dy,
      }),
    );
  };

  const release = () => {
    clearHold();
    if (hold.current !== null) mouse("mouseup", 0, 0);
    hold.current = null;
  };

  useEffect(() => {
    const element = screen.current;
    if (!element) return;
    const observer = new ResizeObserver(() => setView(view.current));
    observer.observe(element);
    drawCursor();
    return () => {
      observer.disconnect();
      clearHold();
    };
  }, []);

  return (
    <>
      <div
        class={`browser-touch-layer${disabled ? " disabled" : ""}`}
        aria-label="Browser trackpad"
        onPointerDown={(event) => {
          if (disabled || !rfb.current) return;
          event.preventDefault();
          event.currentTarget.setPointerCapture(event.pointerId);
          const rect = event.currentTarget.getBoundingClientRect();
          const point = {
            x: event.clientX - rect.left,
            y: event.clientY - rect.top,
            startX: event.clientX - rect.left,
            startY: event.clientY - rect.top,
          };
          pointers.current.set(event.pointerId, point);
          if (pointers.current.size === 1) {
            gesture.current = {
              started: Date.now(),
              moved: false,
              two: false,
              startDistance: 0,
              startMidX: point.x,
              startMidY: point.y,
              lastMidY: point.y,
              view: { ...view.current },
            };
            holdTimer.current = window.setTimeout(() => {
              if (pointers.current.size !== 1 || gesture.current?.moved) return;
              hold.current = event.pointerId;
              mouse("mousedown", 0, 1);
            }, LONG_PRESS_MS);
          } else if (pointers.current.size === 2 && gesture.current) {
            clearHold();
            if (hold.current !== null) release();
            const points = [...pointers.current.values()];
            const mid = midpoint(points);
            gesture.current.two = true;
            gesture.current.startDistance = distance(points);
            gesture.current.startMidX = mid.x;
            gesture.current.startMidY = mid.y;
            gesture.current.lastMidY = mid.y;
            gesture.current.view = { ...view.current };
          } else if (gesture.current) {
            gesture.current.moved = true;
          }
        }}
        onPointerMove={(event) => {
          const point = pointers.current.get(event.pointerId);
          const current = gesture.current;
          if (!point || !current || disabled) return;
          const rect = event.currentTarget.getBoundingClientRect();
          const x = event.clientX - rect.left;
          const y = event.clientY - rect.top;
          const dx = x - point.x;
          const dy = y - point.y;
          point.x = x;
          point.y = y;
          if (pointers.current.size === 1 && !current.two) {
            if (
              Math.hypot(x - point.startX, y - point.startY) > MOVE_THRESHOLD
            ) {
              current.moved = true;
              clearHold();
            }
            moveCursor(dx, dy);
            return;
          }
          if (pointers.current.size !== 2) return;
          const points = [...pointers.current.values()];
          const mid = midpoint(points);
          const ratio =
            current.startDistance > 0
              ? distance(points) / current.startDistance
              : 1;
          const nextScale = Math.max(
            1,
            Math.min(MAX_SCALE, current.view.scale * ratio),
          );
          if (current.view.scale === 1 && Math.abs(nextScale - 1) < 0.04) {
            wheel((current.lastMidY - mid.y) * 2);
            current.lastMidY = mid.y;
          } else {
            const anchorX =
              (current.startMidX - current.view.x) / current.view.scale;
            const anchorY =
              (current.startMidY - current.view.y) / current.view.scale;
            setView({
              scale: nextScale,
              x: mid.x - anchorX * nextScale,
              y: mid.y - anchorY * nextScale,
            });
          }
          if (
            Math.abs(mid.x - current.startMidX) > MOVE_THRESHOLD ||
            Math.abs(mid.y - current.startMidY) > MOVE_THRESHOLD ||
            Math.abs(ratio - 1) > 0.04
          ) {
            current.moved = true;
          }
        }}
        onPointerUp={(event) => {
          const current = gesture.current;
          const count = pointers.current.size;
          if (
            current &&
            !current.moved &&
            Date.now() - current.started < TAP_MS
          ) {
            const button = current.two && count === 2 ? 2 : 0;
            mouse("mousedown", button, button === 0 ? 1 : 2);
            mouse("mouseup", button, 0);
            current.moved = true;
          }
          pointers.current.delete(event.pointerId);
          if (pointers.current.size === 0) {
            release();
            gesture.current = null;
          }
        }}
        onPointerCancel={(event) => {
          if (gesture.current) gesture.current.moved = true;
          pointers.current.delete(event.pointerId);
          if (pointers.current.size === 0) {
            release();
            gesture.current = null;
          }
        }}
      />
      {!disabled && (
        <span
          class="browser-touch-cursor"
          style={cursorStyle}
          aria-hidden="true"
        />
      )}
    </>
  );
}
