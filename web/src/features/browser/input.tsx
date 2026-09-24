import type { RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import BrowserTrackpad from "./trackpad.tsx";
import BrowserViewport from "./viewport.tsx";
import { drawCursor, hiddenCursor, type CursorShape } from "./cursor.ts";

// The pointer scales with the screen like a native one; below this it stops
// shrinking, so it stays findable on a phone.
const POINTER_MIN_SCALE = 0.45;

export default function BrowserInput({
  screen,
  target,
  disabled,
  showTrackpad,
  readOnly,
  cursorShape,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
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

  const canvas = () => target.current?.querySelector("canvas");
  const buttonMask = (button: number) =>
    button === 0 ? 1 : button === 2 ? 2 : 4;
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
  const mouse = (
    kind: "mousedown" | "mouseup" | "mousemove",
    button = 0,
    buttons = 0,
  ) => {
    const point = pointerPoint();
    if (!point) return;
    point.element.dispatchEvent(
      new MouseEvent(kind, {
        bubbles: true,
        cancelable: true,
        clientX: point.x,
        clientY: point.y,
        button,
        buttons,
      }),
    );
  };
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
    const button = pressedButton.current;
    if (disabled) return;
    mouse("mousemove", 0, button === null ? 0 : buttonMask(button));
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
  const wheel = (dy: number) => {
    const point = pointerPoint();
    if (!point || disabled) return;
    point.element.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: point.x,
        clientY: point.y,
        deltaY: dy,
      }),
    );
  };
  const press = (button: number) => {
    if (disabled || pressedButton.current !== null) return;
    pressedButton.current = button;
    mouse("mousedown", button, buttonMask(button));
  };
  const release = () => {
    const button = pressedButton.current;
    if (button === null) return;
    mouse("mouseup", button, 0);
    pressedButton.current = null;
    refreshPointer();
  };
  const click = (button: number) => {
    if (disabled) return;
    mouse("mousedown", button, buttonMask(button));
    mouse("mouseup", button, 0);
    refreshPointer();
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
