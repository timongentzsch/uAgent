// A zoomable view of a surface: scale around a point and pan, never smaller
// than the surface nor past its edges. Shared by the image viewer and the
// remote browser screen.
export interface ZoomView {
  scale: number;
  x: number;
  y: number;
}

export interface Point {
  x: number;
  y: number;
}

export const MAXIMUM_ZOOM = 5;

export const distance = (points: Point[]) =>
  Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y);

export const midpoint = (points: Point[]) => ({
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

const clamp = (value: number, minimum: number, maximum: number) =>
  Math.max(minimum, Math.min(maximum, value));

export function constrainView(
  view: ZoomView,
  width: number,
  height: number,
): ZoomView {
  const scale = clamp(view.scale, 1, MAXIMUM_ZOOM);
  return {
    scale,
    x: clamp(view.x, width * (1 - scale), 0),
    y: clamp(view.y, height * (1 - scale), 0),
  };
}

export function panView(
  view: ZoomView,
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

// Scales by `scaleRatio` keeping the content under `start` under `current`:
// a pinch that also pans, or (start = current) a zoom around one point.
export function pinchView(
  view: ZoomView,
  width: number,
  height: number,
  start: Point,
  current: Point,
  scaleRatio: number,
) {
  const scale = clamp(view.scale * scaleRatio, 1, MAXIMUM_ZOOM);
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
