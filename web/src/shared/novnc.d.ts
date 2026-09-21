declare module "@novnc/novnc" {
  export default class RFB extends EventTarget {
    constructor(target: HTMLElement, url: string);
    scaleViewport: boolean;
    clipViewport: boolean;
    dragViewport: boolean;
    resizeSession: boolean;
    clipboardPasteFrom(text: string): void;
    sendKey(keysym: number, code?: string, down?: boolean): void;
    disconnect(): void;
  }
}
