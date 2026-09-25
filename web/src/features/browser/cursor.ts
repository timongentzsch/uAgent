import RFB from "@novnc/novnc";

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

// The noVNC internals the phone viewer relies on, in one place. noVNC 1.7
// has no public API for either; if an upgrade removes them, these checks
// fail closed and the browser tests fail.
interface RfbInternals {
  _cursor?: { change?: Change; _canvas?: HTMLCanvasElement };
  _sock?: unknown;
  _rfbConnectionState?: string;
}
const internals = (rfb: RFB) => rfb as unknown as RfbInternals;

// noVNC hands the server's pointer to its internal Cursor through change()
// and has no public event for it. On touch devices that Cursor paints its
// own fixed canvas into document.body at the remote's native size, which
// shows behind the modal; the viewer hides it and draws the pointer itself,
// in a layer of the scaled display, as Guacamole does.
export function observeCursor(rfb: RFB, onShape: (shape: CursorShape) => void) {
  const cursor = internals(rfb)._cursor;
  const change = cursor?.change;
  if (!cursor || typeof change !== "function") return;
  if (cursor._canvas) cursor._canvas.style.display = "none";
  cursor.change = (rgba, hotX, hotY, width, height) => {
    change.call(cursor, rgba, hotX, hotY, width, height);
    onShape({ rgba, hotX, hotY, width, height });
  };
}

// The trackpad's pointer, sent the way a native client sends it: one RFB
// PointerEvent at an exact framebuffer pixel. Going through noVNC's mouse
// handling instead meant synthetic DOM events, a round trip through its
// scaling, its move throttle and a document-wide capture on press.
export function sendPointer(rfb: RFB, x: number, y: number, mask: number) {
  const { _sock: sock, _rfbConnectionState: state } = internals(rfb);
  if (rfb.viewOnly || state !== "connected" || !sock) return;
  RFB.messages.pointerEvent(sock, Math.round(x), Math.round(y), mask);
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
