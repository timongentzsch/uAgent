import { longPressMs } from "../../shared/limits.ts";
import type { Point } from "../../shared/zoom.ts";

export const BROWSER_GESTURE = Object.freeze({
  movementSlopPx: 8,
  holdMs: longPressMs,
  tapMs: 300,
  // Finger travel per wheel notch; a notch scrolls Chrome about 100px.
  wheelStepPx: 40,
});

// X11 wheel buttons as RFB mask bits: 4 up, 5 down, 6 left, 7 right.
export const WHEEL = {
  up: 1 << 3,
  down: 1 << 4,
  left: 1 << 5,
  right: 1 << 6,
};

// Whole wheel notches, natural-scroll style, and the unspent travel.
export function wheelNotches(travel: Point) {
  const step = BROWSER_GESTURE.wheelStepPx;
  const whole = (value: number) => Math.trunc(value / step);
  const across = whole(travel.x);
  const along = whole(travel.y);
  const masks: number[] = [];
  for (let i = 0; i < Math.abs(along); i++)
    masks.push(along < 0 ? WHEEL.down : WHEEL.up);
  for (let i = 0; i < Math.abs(across); i++)
    masks.push(across < 0 ? WHEEL.right : WHEEL.left);
  return {
    masks,
    rest: { x: travel.x - across * step, y: travel.y - along * step },
  };
}
