import type { RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import BrowserTrackpad from "./trackpad.tsx";
import BrowserViewport from "./viewport.tsx";
import { drawCursor, hiddenCursor, type CursorShape } from "./cursor.ts";

// The pointer scales with the screen like a native one; below this it stops
// shrinking, so it stays findable on a phone.
const POINTER_MIN_SCALE = 0.45;
// One wheel notch per this many trackpad pixels, as noVNC counts a wheel.
const WHEEL_STEP_PX = 50;

export default function BrowserInput({
  screen,
  target,
  pointer,
  disabled,
  showTrackpad,
  readOnly,
  cursorShape,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
  // Where the trackpad's pointer goes: a framebuffer pixel and held buttons.
  pointer: (x: number, y: number, mask: number) => void;
  disabled: boolean;
  showTrackpad: boolean;
  readOnly: boolean;
  // The server's pointer; null until it sends one.
  cursorShape: CursorShape | null;
}) {
  const cursor = useRef({ x: 0.5, y: 0.5 });
  const marker = useRef<HTMLCanvasElement>(null);
  const reveal = useRef<(point: { x: number; y: number }) => void>(() => {});
  const pressedButton = useRef<number | null>(null);
  const wheelDelta = useRef(0);

  const canvas = () => target.current?.querySelector("canvas");
  // RFB button bits: 1 left, 2 middle, 4 right (DOM numbers right as 2).
  const buttonMask = (button: number) =>
    button === 0 ? 1 : button === 2 ? 4 : 2;
  const pointerPoint = () => {
    const element = canvas();
    const visible = screen.current?.getBoundingClientRect();
    const rect = element?.getBoundingClientRect();
    if (!element || !visible || !rect || !rect.width || !rect.height)
      return null;
    return {
      element,
      x: rect.left + cursor.current.x * rect.width,
      y: rect.top + cursor.current.y * rect.height,
      rect,
      visible,
    };
  };
  // The pointer at its exact framebuffer pixel, with the given buttons held.
  const send = (mask: number) => {
    const element = canvas();
    if (!element) return;
    pointer(
      Math.round(cursor.current.x * (element.width - 1)),
      Math.round(cursor.current.y * (element.height - 1)),
      mask,
    );
  };
  const held = () =>
    pressedButton.current === null ? 0 : buttonMask(pressedButton.current);
  const paintPointer = () => {
    const point = pointerPoint();
    if (!point || !marker.current) return;
    const scale = Math.min(
      1,
      Math.max(POINTER_MIN_SCALE, point.rect.width / point.element.width),
    );
    const shape = marker.current;
    const hotX = (cursorShape?.hotX ?? 1) * scale,
      hotY = (cursorShape?.hotY ?? 1) * scale;
    shape.style.width = `${shape.width * scale}px`;
    shape.style.height = `${shape.height * scale}px`;
    shape.style.transform = `translate(${point.x - point.visible.left - hotX}px, ${point.y - point.visible.top - hotY}px)`;
    shape.dataset.hotspot = `${hotX},${hotY}`;
  };
  const refreshPointer = () => {
    paintPointer();
    if (!disabled) send(held());
  };
  const move = (dx: number, dy: number) => {
    if (disabled) return;
    const point = pointerPoint();
    if (!point) return;
    cursor.current.x += dx / point.rect.width;
    cursor.current.y += dy / point.rect.height;
    cursor.current.x = Math.max(
      0,
      Math.min(1 - 1 / point.element.width, cursor.current.x),
    );
    cursor.current.y = Math.max(
      0,
      Math.min(1 - 1 / point.element.height, cursor.current.y),
    );
    const next = pointerPoint();
    if (next) reveal.current({ x: next.x, y: next.y });
    refreshPointer();
  };
  // VNC scrolls in notches: a press and release of button 4 (up) or 5 (down).
  const wheel = (dy: number) => {
    if (disabled) return;
    wheelDelta.current += dy;
    while (Math.abs(wheelDelta.current) >= WHEEL_STEP_PX) {
      const down = wheelDelta.current > 0;
      send(held() | (down ? 1 << 4 : 1 << 3));
      send(held());
      wheelDelta.current -= down ? WHEEL_STEP_PX : -WHEEL_STEP_PX;
    }
  };
  const press = (button: number) => {
    if (disabled || pressedButton.current !== null) return;
    pressedButton.current = button;
    send(buttonMask(button));
  };
  const release = () => {
    if (pressedButton.current === null) return;
    pressedButton.current = null;
    send(0);
  };
  const click = (button: number) => {
    if (disabled) return;
    send(buttonMask(button));
    send(0);
  };

  useEffect(() => {
    if (disabled) release();
    return release;
  }, [disabled]);
  useEffect(() => {
    if (!marker.current) return;
    drawCursor(marker.current, cursorShape);
    paintPointer();
  }, [cursorShape]);

  return (
    <>
      <BrowserViewport
        screen={screen}
        target={target}
        readOnly={readOnly}
        refreshPointer={paintPointer}
        reveal={reveal}
        trackpad={showTrackpad && !disabled}
        pointAt={(x, y) => {
          const point = pointerPoint();
          if (!point) return;
          cursor.current = {
            x: (x - point.rect.left) / point.rect.width,
            y: (y - point.rect.top) / point.rect.height,
          };
          paintPointer();
        }}
      >
        <canvas
          ref={marker}
          class="browser-pointer"
          aria-hidden="true"
          hidden={
            !showTrackpad ||
            disabled ||
            (!!cursorShape && hiddenCursor(cursorShape))
          }
        />
      </BrowserViewport>
      {showTrackpad && (
        <BrowserTrackpad
          disabled={disabled}
          move={move}
          wheel={wheel}
          click={click}
          press={press}
          release={release}
        />
      )}
    </>
  );
}
