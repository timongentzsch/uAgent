import { useRef, useState } from "preact/hooks";

interface Point {
  x: number;
  y: number;
  startX: number;
  startY: number;
}

interface Gesture {
  started: number;
  moved: boolean;
  two: boolean;
  lastMidY: number;
}

export default function Trackpad({
  onMove,
  onButton,
  onScroll,
  disabled,
}: {
  onMove: (dx: number, dy: number) => void;
  onButton: (button: 0 | 2, down: boolean) => void;
  onScroll: (dy: number) => void;
  disabled: boolean;
}) {
  const pointers = useRef(new Map<number, Point>());
  const gesture = useRef<Gesture | null>(null);
  const hold = useRef<number | null>(null);
  const [holding, setHolding] = useState(false);

  const release = (id: number) => {
    if (hold.current !== id) return;
    hold.current = null;
    setHolding(false);
    onButton(0, false);
  };

  return (
    <div class="browser-trackpad" aria-label="Browser trackpad">
      <div
        class="browser-trackpad-surface"
        aria-label="Move cursor; tap to click; use two fingers to scroll or right-click"
        onPointerDown={(event) => {
          if (disabled) return;
          event.preventDefault();
          event.currentTarget.setPointerCapture(event.pointerId);
          const point = {
            x: event.clientX,
            y: event.clientY,
            startX: event.clientX,
            startY: event.clientY,
          };
          pointers.current.set(event.pointerId, point);
          if (pointers.current.size === 1) {
            gesture.current = {
              started: Date.now(),
              moved: false,
              two: false,
              lastMidY: point.y,
            };
          } else if (pointers.current.size === 2 && gesture.current) {
            gesture.current.two = true;
            gesture.current.lastMidY =
              [...pointers.current.values()].reduce((sum, p) => sum + p.y, 0) /
              2;
          }
        }}
        onPointerMove={(event) => {
          const point = pointers.current.get(event.pointerId);
          const current = gesture.current;
          if (!point || !current || disabled) return;
          const dx = event.clientX - point.x;
          const dy = event.clientY - point.y;
          point.x = event.clientX;
          point.y = event.clientY;
          if (pointers.current.size === 1 && !current.two) {
            if (Math.hypot(point.x - point.startX, point.y - point.startY) > 8)
              current.moved = true;
            onMove(dx, dy);
          } else if (pointers.current.size === 2) {
            const midY =
              [...pointers.current.values()].reduce((sum, p) => sum + p.y, 0) /
              2;
            const scroll = midY - current.lastMidY;
            current.lastMidY = midY;
            if (Math.abs(scroll) > 2) current.moved = true;
            onScroll(scroll);
          }
        }}
        onPointerUp={(event) => {
          const current = gesture.current;
          if (pointers.current.has(event.pointerId) && current && !disabled) {
            if (!current.moved && Date.now() - current.started < 300) {
              if (pointers.current.size === 2 && current.two) {
                onButton(2, true);
                onButton(2, false);
              } else if (
                pointers.current.size === 1 &&
                !current.two &&
                hold.current === null
              ) {
                onButton(0, true);
                onButton(0, false);
              }
            }
          }
          pointers.current.delete(event.pointerId);
          gesture.current = null;
        }}
        onPointerCancel={(event) => {
          pointers.current.delete(event.pointerId);
          gesture.current = null;
        }}
      >
        <small>
          Move · tap to click · two fingers to scroll or right-click
        </small>
      </div>
      <div class="browser-trackpad-buttons">
        <button
          type="button"
          class={holding ? "active" : ""}
          disabled={disabled}
          onPointerDown={(event) => {
            event.preventDefault();
            event.currentTarget.setPointerCapture(event.pointerId);
            hold.current = event.pointerId;
            setHolding(true);
            onButton(0, true);
          }}
          onPointerUp={(event) => release(event.pointerId)}
          onPointerCancel={(event) => release(event.pointerId)}
          onLostPointerCapture={(event) => release(event.pointerId)}
        >
          Left · hold
        </button>
        <button
          type="button"
          disabled={disabled}
          onClick={() => {
            onButton(2, true);
            onButton(2, false);
          }}
        >
          Right
        </button>
      </div>
    </div>
  );
}
