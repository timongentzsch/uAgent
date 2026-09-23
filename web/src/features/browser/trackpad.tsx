import { useEffect, useRef } from "preact/hooks";
import {
  acceleratedPointerDelta,
  BROWSER_GESTURE,
  capturePointer,
  midpoint,
  type Point,
  type TrackedPoint,
} from "./gestures.ts";

function BrowserTrackpad({
  disabled,
  move,
  wheel,
  click,
  press,
  release,
}: {
  disabled: boolean;
  move: (dx: number, dy: number) => void;
  wheel: (dy: number) => void;
  click: (button: number) => void;
  press: (button: number) => void;
  release: () => void;
}) {
  const actions = useRef({ move, wheel, click, press, release });
  actions.current = { move, wheel, click, press, release };
  const pointers = useRef(new Map<number, TrackedPoint>());
  const gesture = useRef<{
    started: number;
    moved: boolean;
    two: boolean;
    lastMidpoint: Point;
  } | null>(null);
  const motion = useRef({ x: 0, y: 0, wheel: 0 });
  const frame = useRef(0);

  const flush = () => {
    cancelAnimationFrame(frame.current);
    frame.current = 0;
    const pending = motion.current;
    motion.current = { x: 0, y: 0, wheel: 0 };
    if (pending.x || pending.y) actions.current.move(pending.x, pending.y);
    if (pending.wheel) actions.current.wheel(pending.wheel);
  };
  const schedule = () => {
    if (!frame.current) frame.current = requestAnimationFrame(flush);
  };
  const finish = (event: PointerEvent) => {
    const current = gesture.current;
    const count = pointers.current.size;
    flush();
    if (
      current &&
      !current.moved &&
      event.timeStamp - current.started <= BROWSER_GESTURE.tapDurationMs
    ) {
      actions.current.click(current.two && count === 2 ? 2 : 0);
      current.moved = true;
    }
    pointers.current.delete(event.pointerId);
    if (pointers.current.size === 0) gesture.current = null;
  };

  useEffect(
    () => () => {
      flush();
      actions.current.release();
    },
    [],
  );
  useEffect(() => {
    const stop = () => {
      flush();
      pointers.current.clear();
      gesture.current = null;
      actions.current.release();
    };
    const hide = () => {
      if (document.visibilityState === "hidden") stop();
    };
    addEventListener("blur", stop);
    document.addEventListener("visibilitychange", hide);
    return () => {
      removeEventListener("blur", stop);
      document.removeEventListener("visibilitychange", hide);
    };
  }, []);
  useEffect(() => {
    if (!disabled) return;
    pointers.current.clear();
    gesture.current = null;
    flush();
    actions.current.release();
  }, [disabled]);

  const pointerButton = (button: number) => ({
    onPointerDown: (event: PointerEvent) => {
      if (disabled) return;
      event.preventDefault();
      capturePointer(event.currentTarget as HTMLButtonElement, event.pointerId);
      flush();
      actions.current.press(button);
    },
    onPointerUp: (event: PointerEvent) => {
      event.preventDefault();
      flush();
      actions.current.release();
    },
    onPointerCancel: () => {
      flush();
      actions.current.release();
    },
  });

  return (
    <div class={`browser-trackpad${disabled ? " disabled" : ""}`}>
      <div
        class="browser-trackpad-surface"
        aria-label="Browser trackpad"
        onPointerDown={(event) => {
          if (disabled) return;
          event.preventDefault();
          capturePointer(event.currentTarget, event.pointerId);
          const point = {
            x: event.clientX,
            y: event.clientY,
            startX: event.clientX,
            startY: event.clientY,
            time: event.timeStamp,
          };
          pointers.current.set(event.pointerId, point);
          if (pointers.current.size === 1) {
            gesture.current = {
              started: event.timeStamp,
              moved: false,
              two: false,
              lastMidpoint: point,
            };
          } else if (pointers.current.size === 2 && gesture.current) {
            flush();
            gesture.current.two = true;
            gesture.current.lastMidpoint = midpoint([
              ...pointers.current.values(),
            ]);
          } else if (gesture.current) {
            gesture.current.moved = true;
          }
        }}
        onPointerMove={(event) => {
          const point = pointers.current.get(event.pointerId);
          const current = gesture.current;
          if (!point || !current || disabled) return;
          event.preventDefault();
          const dx = event.clientX - point.x;
          const dy = event.clientY - point.y;
          const elapsed = event.timeStamp - point.time;
          point.x = event.clientX;
          point.y = event.clientY;
          point.time = event.timeStamp;
          if (
            Math.hypot(point.x - point.startX, point.y - point.startY) >
            BROWSER_GESTURE.movementSlopPx
          )
            current.moved = true;
          if (pointers.current.size === 1 && !current.two) {
            const delta = acceleratedPointerDelta(dx, dy, elapsed);
            motion.current.x += delta.x;
            motion.current.y += delta.y;
            schedule();
            return;
          }
          if (pointers.current.size !== 2) return;
          const next = midpoint([...pointers.current.values()]);
          motion.current.wheel +=
            (current.lastMidpoint.y - next.y) * BROWSER_GESTURE.wheelGain;
          current.lastMidpoint = next;
          schedule();
        }}
        onPointerUp={finish}
        onPointerCancel={(event) => {
          if (gesture.current) gesture.current.moved = true;
          finish(event);
        }}
      >
        <span>Move pointer · tap to click · two fingers to scroll</span>
      </div>
      <div class="browser-trackpad-buttons">
        <button type="button" disabled={disabled} {...pointerButton(0)}>
          Left
        </button>
        <button type="button" disabled={disabled} {...pointerButton(2)}>
          Right
        </button>
      </div>
    </div>
  );
}

export default BrowserTrackpad;
