import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { useApi } from "../state/api.ts";
import { useLatest } from "./use-latest.ts";
import { useResource } from "./use-resource.ts";
import { pageSize } from "./page-size.ts";
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
  // A slide's speaker notes.
  notes?: string;
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
  | { type: "zoom"; by: number; x: number; y: number }
  // A document's page breaks, its edited block, its text as Markdown, and
  // where it is scrolled to (`end`: as far as it goes; `after`: the last
  // goto it was told).
  | { type: "pages"; breaks: number[] }
  | { type: "edit"; index: number; before: string; html: string }
  | { type: "markdown"; text: string }
  | { type: "at"; top: number; end: boolean; after: number }
  // How many occurrences a find marked.
  | { type: "find"; count: number };

// Tells a page what to do: select on or off, set a tweak, scroll a document
// to an offset (its `n`th goto, counted from 1 for each document loaded), or
// mark the words found and bring the `index`th into view.
export const tell = (
  frame: HTMLIFrameElement | null,
  message:
    | { type: "select"; on: boolean }
    | { type: "edit"; on: boolean }
    | { type: "markdown" }
    | { type: "tweak"; name: string; value: unknown }
    | { type: "goto"; top: number; n: number }
    | { type: "find"; text: string; index: number },
) => frame?.contentWindow?.postMessage({ uagentDesign: 1, ...message }, "*");

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
  far,
  media,
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
  // Only its box, with nothing in it. The box itself stays whatever it
  // holds: a legend whose boxes came and went with their pages lost its
  // place as they were swapped.
  far?: boolean;
  // An "image" or a "video" of the folder is drawn as itself: it says its
  // size as a page does once it has loaded, or that it did not.
  media?: string;
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
  const said = (w: number, h: number) =>
    onMessage?.({
      type: "ready",
      title: "",
      artboard: { w, h },
      tweaks: null,
      height: h,
    });
  const lost = () =>
    onMessage?.({ type: "error", message: "did not load", line: 0 });
  return (
    <div
      class="page-frame"
      data-live={live || undefined}
      // A picture stands beside what names and opens it.
      aria-hidden={!live || undefined}
      style={
        sized
          ? { width: `${width * scale}px`, height: `${height * scale}px` }
          : undefined
      }
    >
      {far ? null : media === "image" ? (
        <img
          src={src}
          alt=""
          onLoad={({ currentTarget: image }) =>
            said(image.naturalWidth, image.naturalHeight)
          }
          onError={lost}
        />
      ) : media === "video" ? (
        <video
          src={src}
          controls
          preload="metadata"
          onLoadedMetadata={({ currentTarget: film }) =>
            said(film.videoWidth, film.videoHeight)
          }
          onError={lost}
        />
      ) : (
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
      )}
    </div>
  );
}

// The address a conversation's folder is previewed from, minted once.
export function usePreviewBase(session: string) {
  const { api, routes } = useApi();
  return useResource(
    () => api<{ base: string }>(routes.preview(session), {}),
    [session],
  );
}

// One board as a picture: its page, or an image or a video of the folder,
// drawn `w` by `h` at a scale. While `far` (out of a legend's view) only its box is
// there, so a long deck runs the few pages near the view and no more.
export function BoardFrame({
  src,
  title,
  w,
  h,
  scale,
  far,
  media,
  onMessage,
}: {
  src: string;
  title: string;
  w: number;
  h: number;
  scale?: number;
  far?: boolean;
  media?: string;
  onMessage?: (message: PageMessage) => void;
}) {
  return (
    <Frame
      src={src}
      title={title}
      width={w}
      height={h}
      scale={scale}
      live={false}
      far={far}
      media={media}
      onMessage={onMessage}
    />
  );
}

// How tall every page thumbnail is, in chat and the Library alike.
const THUMB_HEIGHT = 160;

// A page small and still: drawn at its own size (a tall one, its top) and
// scaled to one height, so thumbnails line up whatever their pages.
export function PageThumb({ base, path }: { base: string; path: string }) {
  const [info, setInfo] = useState<PageInfo>();
  const { w, h } = pageSize(info, 900);
  return (
    <div class="page-thumb">
      <BoardFrame
        src={pageAddress(base, path)}
        title={info?.title || path}
        w={w}
        h={h}
        scale={THUMB_HEIGHT / h}
        onMessage={(message) => message.type === "ready" && setInfo(message)}
      />
    </div>
  );
}
