// A zoomable view of a surface: scale around a point and pan. Media and the
// remote browser screen are never smaller than the surface nor past its
// edges; a plane with no edges, such as the design canvas, zooms in a range
// of its own.
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
// Past this a press is a drag rather than a tap.
export const TAP_SLOP_PX = 8;

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
// With a `range`, the view has no edges and its scale stays in that range.
export function pinchView(
  view: ZoomView,
  width: number,
  height: number,
  start: Point,
  current: Point,
  scaleRatio: number,
  range?: [number, number],
) {
  const [least, most] = range ?? [1, MAXIMUM_ZOOM];
  const scale = clamp(view.scale * scaleRatio, least, most);
  const next = {
    scale,
    x: current.x - ((start.x - view.x) / view.scale) * scale,
    y: current.y - ((start.y - view.y) / view.scale) * scale,
  };
  return range ? next : constrainView(next, width, height);
}

// Two fingers as a pinch began: their spread and middle, and the view then.
export interface Pinch {
  distance: number;
  midpoint: Point;
  view: ZoomView;
}

export const pinchStart = (points: Point[], view: ZoomView): Pinch => ({
  distance: distance(points),
  midpoint: midpoint(points),
  view: { ...view },
});

// The view once those fingers have moved to `points`.
export const pinchTo = (
  start: Pinch,
  points: Point[],
  width: number,
  height: number,
  range?: [number, number],
) =>
  pinchView(
    start.view,
    width,
    height,
    start.midpoint,
    midpoint(points),
    start.distance > 0 ? distance(points) / start.distance : 1,
    range,
  );

// Modifier-wheel zooms at the pointer; ordinary wheel movement belongs to
// the surface (pan a canvas, scroll a page, or pan an enlarged image).
export function bindZoomWheel(
  element: HTMLElement,
  zoom: (ratio: number, point: Point) => void,
  pan?: (dx: number, dy: number) => boolean,
) {
  const wheel = (event: WheelEvent) => {
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      const rect = element.getBoundingClientRect();
      zoom(Math.exp(-event.deltaY * 0.01), {
        x: event.clientX - rect.left,
        y: event.clientY - rect.top,
      });
    } else if (pan?.(-event.deltaX, -event.deltaY)) event.preventDefault();
  };
  element.addEventListener("wheel", wheel, { passive: false });
  return () => element.removeEventListener("wheel", wheel);
}
