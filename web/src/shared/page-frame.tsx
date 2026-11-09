import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { api, routes } from "../state/api.ts";
import { useLatest } from "./use-latest.ts";
import { useResource } from "./use-resource.ts";
import "./page-frame.css";

// One page of the folder, live in a sandboxed frame. It runs in an opaque
// origin and is served from a preview address, so it can reach neither this
// page nor the host's API; the helper the host puts first in it reports what
// it is and what failed (web/public/design-helper.js).

export interface PageInfo {
  title: string;
  // A fixed artboard's size; 0 × 0 for a page, which is as tall as it is. A
  // document is its paper's width.
  artboard: { w: number; h: number; slide?: boolean };
  // A slide of a deck, a document on paper, or any other design.
  kind?: "slide" | "doc" | "design";
  tweaks: Record<string, unknown> | null;
  height: number;
  // Its stylesheets' addresses.
  sheets?: string[];
}
export interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}
export interface Pick {
  rect: Rect;
  selector: string;
  tag: string;
  text: string;
}
export type PageMessage =
  | ({ type: "ready" } & PageInfo)
  | { type: "size"; height: number }
  | { type: "error"; message: string; line: number }
  | { type: "hover" | "picked"; rect: Rect }
  | ({ type: "pick" } & Pick)
  | { type: "key"; key: string }
  | { type: "zoom"; by: number; x: number; y: number };

// Tells a page what to do: select on or off, or set a tweak.
export const tell = (
  frame: HTMLIFrameElement | null,
  message:
    | { type: "select"; on: boolean }
    | { type: "tweak"; name: string; value: unknown },
) => frame?.contentWindow?.postMessage({ uagentDesign: 1, ...message }, "*");

// A page is drawn this wide when it names no size.
const PAGE_WIDTH = 1440;
// How tall a page's frame may grow on the canvas.
const PAGE_HEIGHT_MAX = 4000;

// The size a page is drawn at: its artboard, or as wide as it says (else
// PAGE_WIDTH) and as tall as it measured, up to `most`.
export function pageSize(info?: PageInfo, most = PAGE_HEIGHT_MAX) {
  const fixed = !!info?.artboard.w && !!info.artboard.h;
  return {
    fixed,
    w: info?.artboard.w || PAGE_WIDTH,
    h: fixed ? info!.artboard.h : Math.min(most, info?.height || 900),
  };
}
// A file of the folder at its preview address.
export const pageAddress = (base: string, path: string) =>
  base + path.split("/").map(encodeURIComponent).join("/");

export default function Frame({
  src,
  title,
  width,
  height,
  scale = 1,
  live = true,
  onMessage,
  frameRef,
}: {
  src: string;
  title: string;
  // Its size in CSS pixels; undefined fills the box it sits in.
  width?: number;
  height?: number;
  scale?: number;
  // On the canvas a frame is a picture: the canvas takes the pointer.
  live?: boolean;
  onMessage?: (message: PageMessage) => void;
  // The frame itself, to tell the page things.
  frameRef?: { current: HTMLIFrameElement | null };
}) {
  const own = useRef<HTMLIFrameElement>(null);
  const frame = frameRef ?? own;
  const handle = useLatest(onMessage);
  // biome-ignore lint/correctness/useExhaustiveDependencies: one listener per frame, before the page can answer
  useLayoutEffect(() => {
    const heard = (event: MessageEvent) => {
      if (event.source !== frame.current?.contentWindow) return;
      if (event.data?.uagentDesign === 1) handle.current?.(event.data);
    };
    addEventListener("message", heard);
    return () => removeEventListener("message", heard);
  }, []);
  const sized = width !== undefined && height !== undefined;
  return (
    <div
      class="page-frame"
      data-live={live || undefined}
      style={
        sized
          ? { width: `${width * scale}px`, height: `${height * scale}px` }
          : undefined
      }
    >
      <iframe
        ref={frame}
        src={src}
        title={title}
        sandbox="allow-scripts allow-forms"
        tabIndex={live ? undefined : -1}
        style={
          sized
            ? {
                width: `${width}px`,
                height: `${height}px`,
                transform: `scale(${scale})`,
              }
            : undefined
        }
      />
    </div>
  );
}

// The address a conversation's folder is previewed from, minted once.
export const usePreviewBase = (session: string) =>
  useResource(
    () => api<{ base: string }>(routes.preview(session), {}),
    [session],
  );

// How tall every page thumbnail is, in chat and the Library alike.
const THUMB_HEIGHT = 160;

// A page small and still: drawn at its own size (a tall one, its top) and
// scaled to one height, so thumbnails line up whatever their pages.
export function PageThumb({ base, path }: { base: string; path: string }) {
  const [info, setInfo] = useState<PageInfo>();
  const { w, h } = pageSize(info, 900);
  return (
    <div class="page-thumb">
      <Frame
        src={pageAddress(base, path)}
        title={info?.title || path}
        width={w}
        height={h}
        scale={THUMB_HEIGHT / h}
        live={false}
        onMessage={(message) => message.type === "ready" && setInfo(message)}
      />
    </div>
  );
}
