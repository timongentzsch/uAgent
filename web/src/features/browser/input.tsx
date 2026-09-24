import type { RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import BrowserTrackpad from "./trackpad.tsx";
import BrowserViewport from "./viewport.tsx";

// The arrow's size in remote pixels, as Chrome draws its own pointer.
const POINTER_WIDTH = 20;
const POINTER_HEIGHT = 24;
const POINTER_MIN_SCALE = 0.45;

export default function BrowserInput({
  screen,
  target,
  disabled,
  showTrackpad,
  readOnly,
}: {
  screen: RefObject<HTMLDivElement>;
  target: RefObject<HTMLDivElement>;
  disabled: boolean;
  showTrackpad: boolean;
  readOnly: boolean;
}) {
  const cursor = useRef({ x: 0.5, y: 0.5 });
  const marker = useRef<SVGSVGElement>(null);
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
    marker.current.style.transform = `translate(${point.x - point.visible.left}px, ${point.y - point.visible.top}px)`;
    // Drawn at the remote cursor's own size, so it shrinks with the screen
    // like a native pointer; the floor keeps it findable on a phone.
    const scale = Math.min(
      1,
      Math.max(POINTER_MIN_SCALE, point.rect.width / point.element.width),
    );
    marker.current.style.width = `${POINTER_WIDTH * scale}px`;
    marker.current.style.height = `${POINTER_HEIGHT * scale}px`;
    // At framebuffer edges the hotspot must still reach the last pixel. Turn
    // the arrow inward there instead of clipping its entire shape offscreen.
    const { width, height } = marker.current.getBoundingClientRect();
    marker.current.firstElementChild?.setAttribute(
      "transform",
      `scale(${point.x + width > point.visible.right ? -1 : 1}, ${point.y + height > point.visible.bottom ? -1 : 1})`,
    );
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
        <svg
          ref={marker}
          class="browser-pointer"
          viewBox="0 0 20 24"
          aria-hidden="true"
          hidden={!showTrackpad || disabled}
        >
          <path d="M0 0v17l4.5-4 3.5 7 3-1.5-3.5-7H14Z" />
        </svg>
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
