import type { RefObject } from "preact";
import { useEffect, useRef } from "preact/hooks";
import BrowserTrackpad from "./trackpad.tsx";
import BrowserViewport from "./viewport.tsx";

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
  const refreshPointer = () => {
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
    const minimumX = Math.max(
      0,
      (point.visible.left - point.rect.left) / point.rect.width,
    );
    const maximumX = Math.min(
      1,
      (point.visible.right - point.rect.left) / point.rect.width,
    );
    const minimumY = Math.max(
      0,
      (point.visible.top - point.rect.top) / point.rect.height,
    );
    const maximumY = Math.min(
      1,
      (point.visible.bottom - point.rect.top) / point.rect.height,
    );
    cursor.current.x = Math.max(minimumX, Math.min(maximumX, cursor.current.x));
    cursor.current.y = Math.max(minimumY, Math.min(maximumY, cursor.current.y));
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
    // noVNC's touch cursor checks what is under the release point. The
    // viewport gesture layer is above its canvas, so refresh the real remote
    // cursor after release instead of drawing a second local cursor.
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
      <div
        class={`browser-screen${readOnly ? " readonly" : ""}`}
        ref={screen}
        aria-label={
          readOnly ? "Read-only browser display" : "Interactive browser display"
        }
      >
        <div class="browser-rfb" ref={target} />
        <BrowserViewport
          screen={screen}
          target={target}
          refreshPointer={refreshPointer}
        />
      </div>
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
