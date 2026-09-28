import RFB from "@novnc/novnc";

// The noVNC internals the viewer relies on, in one place. noVNC 1.7 has no
// public API for either; if an upgrade removes them, these fail closed and
// the browser tests fail.
interface RfbInternals {
  _cursor?: { _canvas?: HTMLCanvasElement };
  _sock?: unknown;
  _rfbConnectionState?: string;
}
const internals = (rfb: RFB) => rfb as unknown as RfbInternals;

// On touch devices noVNC paints the remote pointer into its own fixed canvas
// on document.body, behind the modal. Direct touch needs no pointer.
export function hideTouchCursor(rfb: RFB) {
  const canvas = internals(rfb)._cursor?._canvas;
  if (canvas) canvas.style.display = "none";
}

// A pointer event at an exact framebuffer pixel, sent the way a native client
// sends it rather than through noVNC's synthetic mouse handling.
export function sendPointer(rfb: RFB, x: number, y: number, mask: number) {
  const { _sock: sock, _rfbConnectionState: state } = internals(rfb);
  if (rfb.viewOnly || state !== "connected" || !sock) return;
  RFB.messages.pointerEvent(sock, Math.round(x), Math.round(y), mask);
}
