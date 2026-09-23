export interface BrowserView {
  scale: number;
  x: number;
  y: number;
}

export interface Point {
  x: number;
  y: number;
}

export interface TrackedPoint extends Point {
  startX: number;
  startY: number;
  time: number;
}

export const distance = (points: TrackedPoint[]) =>
  Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y);

export const midpoint = (points: TrackedPoint[]) => ({
  x: (points[0].x + points[1].x) / 2,
  y: (points[0].y + points[1].y) / 2,
});

export function capturePointer(element: HTMLElement, pointerId: number) {
  try {
    element.setPointerCapture(pointerId);
  } catch {
    // A cancel can race capture on mobile Safari. The current event still
    // arrives, and the ordinary release/cancel cleanup remains active.
  }
}

// Keep the interaction tuning in one place. These are device-pixel gesture
// thresholds, independent of the interface zoom setting.
export const BROWSER_GESTURE = Object.freeze({
  maximumViewScale: 5,
  cursorEdgeInsetPx: 24,
  movementSlopPx: 8,
  tapDurationMs: 300,
  pointerBaseGain: 0.85,
  pointerAccelerationPerPxMs: 0.55,
  pointerMaximumGain: 3,
  pointerSampleFloorMs: 4,
  wheelGain: 2,
});

const clamp = (value: number, minimum: number, maximum: number) =>
  Math.max(minimum, Math.min(maximum, value));

export function constrainView(
  view: BrowserView,
  width: number,
  height: number,
): BrowserView {
  const scale = clamp(view.scale, 1, BROWSER_GESTURE.maximumViewScale);
  return {
    scale,
    x: clamp(view.x, width * (1 - scale), 0),
    y: clamp(view.y, height * (1 - scale), 0),
  };
}

export function panView(
  view: BrowserView,
  width: number,
  height: number,
  dx: number,
  dy: number,
) {
  return constrainView(
    { scale: view.scale, x: view.x + dx, y: view.y + dy },
    width,
    height,
  );
}

export function pinchView(
  view: BrowserView,
  width: number,
  height: number,
  start: Point,
  current: Point,
  scaleRatio: number,
) {
  const scale = clamp(
    view.scale * scaleRatio,
    1,
    BROWSER_GESTURE.maximumViewScale,
  );
  const anchorX = (start.x - view.x) / view.scale;
  const anchorY = (start.y - view.y) / view.scale;
  return constrainView(
    {
      scale,
      x: current.x - anchorX * scale,
      y: current.y - anchorY * scale,
    },
    width,
    height,
  );
}

export function acceleratedPointerDelta(
  dx: number,
  dy: number,
  elapsedMs: number,
) {
  const speed =
    Math.hypot(dx, dy) /
    Math.max(elapsedMs, BROWSER_GESTURE.pointerSampleFloorMs);
  const gain = clamp(
    BROWSER_GESTURE.pointerBaseGain +
      speed * BROWSER_GESTURE.pointerAccelerationPerPxMs,
    BROWSER_GESTURE.pointerBaseGain,
    BROWSER_GESTURE.pointerMaximumGain,
  );
  return { x: dx * gain, y: dy * gain };
}

// Reveal only the edge the pointer crossed. Manual panning does not call this.
export function followPointer(
  view: BrowserView,
  width: number,
  height: number,
  point: Point,
) {
  const inset = Math.min(
    BROWSER_GESTURE.cursorEdgeInsetPx,
    width / 2,
    height / 2,
  );
  return panView(
    view,
    width,
    height,
    clamp(point.x, inset, width - inset) - point.x,
    clamp(point.y, inset, height - inset) - point.y,
  );
}
