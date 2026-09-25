declare module "@novnc/novnc" {
  export default class RFB extends EventTarget {
    constructor(target: HTMLElement, url: string);
    viewOnly: boolean;
    scaleViewport: boolean;
    clipViewport: boolean;
    dragViewport: boolean;
    resizeSession: boolean;
    clipboardPasteFrom(text: string): void;
    sendKey(keysym: number, code?: string, down?: boolean): void;
    disconnect(): void;
    // The client-to-server message encoders; public on the class.
    static messages: {
      pointerEvent(sock: unknown, x: number, y: number, mask: number): void;
    };
  }
}
