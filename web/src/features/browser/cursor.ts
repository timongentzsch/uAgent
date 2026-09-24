import type RFB from "@novnc/novnc";

// The remote pointer as the RFB Cursor pseudo-encoding delivers it: the image
// Chrome itself shows (arrow, I-beam, hand, resize), with its hotspot.
export interface CursorShape {
  rgba: Uint8Array;
  hotX: number;
  hotY: number;
  width: number;
  height: number;
}

type Change = (
  rgba: Uint8Array,
  hotX: number,
  hotY: number,
  width: number,
  height: number,
) => void;

// noVNC hands the server's pointer to its internal Cursor through change()
// and has no public event for it. On touch devices that Cursor paints a
// fixed canvas under document.body, which a modal dialog covers, so the
// viewer draws the pointer itself -- as Guacamole does, in a layer of the
// scaled display. This is the one private seam; if a noVNC upgrade removes
// it, the viewer keeps its own arrow and the browser test fails.
export function observeCursor(rfb: RFB, onShape: (shape: CursorShape) => void) {
  const cursor = (rfb as unknown as { _cursor?: { change?: Change } })._cursor;
  const change = cursor?.change;
  if (!cursor || typeof change !== "function") return;
  cursor.change = (rgba, hotX, hotY, width, height) => {
    change.call(cursor, rgba, hotX, hotY, width, height);
    onShape({ rgba, hotX, hotY, width, height });
  };
}

// An all-transparent image is how the server hides the pointer, e.g. while
// Chrome takes keyboard input.
export const hiddenCursor = (shape: CursorShape) =>
  !shape.width ||
  !shape.height ||
  !shape.rgba.some((_, i) => i % 4 === 3 && shape.rgba[i]);

// The arrow shown until the server sends its pointer, in remote pixels.
const ARROW = { width: 20, height: 24 };

// Paints `shape` at its own resolution; CSS scales it with the screen.
export function drawCursor(
  canvas: HTMLCanvasElement,
  shape: CursorShape | null,
) {
  const context = canvas.getContext("2d");
  if (!context) return;
  if (shape && !hiddenCursor(shape)) {
    canvas.width = shape.width;
    canvas.height = shape.height;
    context.putImageData(
      new ImageData(
        new Uint8ClampedArray(shape.rgba),
        shape.width,
        shape.height,
      ),
      0,
      0,
    );
    return;
  }
  canvas.width = ARROW.width;
  canvas.height = ARROW.height;
  context.fillStyle = "#111";
  context.strokeStyle = "#fff";
  context.lineWidth = 1.5;
  context.lineJoin = "round";
  const arrow = new Path2D("M1 1v17l4.5-4 3.5 7 3-1.5-3.5-7H15Z");
  context.fill(arrow);
  context.stroke(arrow);
}
