declare module "@novnc/novnc" {
  export default class RFB extends EventTarget {
    constructor(target: HTMLElement, url: string);
    scaleViewport: boolean;
    clipViewport: boolean;
    dragViewport: boolean;
    resizeSession: boolean;
    disconnect(): void;
  }
}
