import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { useApi } from "../state/api.ts";
import { useLatest } from "./use-latest.ts";
import { useResource } from "./use-resource.ts";
import { pageSize } from "./page-size.ts";
import "./page-frame.css";

// One page of the folder, live in a sandboxed frame: in an opaque origin and
// served from a preview address, it reaches neither this page nor the host's
// API. The helper the host puts first in it reports what it is and what failed.

export interface PageInfo {
  title: string;
  // A fixed artboard's size; 0 × 0 for a page, which is as tall as it is. A
  // document's is one sheet of its paper.
  artboard: { w: number; h: number; slide?: boolean };
  // A slide's speaker notes, a line each.
  notes?: string;
  tweaks: Record<string, unknown> | null;
  height: number;
  // Its stylesheets' addresses.
  sheets?: string[];
  // A sheet: its tables drawn as a spreadsheet's grid.
  sheet?: boolean;
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
// How a sheet's cell shows, which the format controls set.
export interface Marks {
  bold: boolean;
  italic: boolean;
  wrap: boolean;
  align: string;
  fill: string;
  ink: string;
  format: string;
}
// A rule of a sheet's column: on the row or on the cell, and where the page
// understands it, what it tests and what it gives.
export interface Rule {
  text: string;
  row: boolean;
  condition?: string;
  effects?: string[];
}
export type PageMessage =
  | ({ type: "ready" } & PageInfo)
  // Loaded, a sheet calculated, and painted: what the frame waits for.
  | { type: "drawn" }
  | { type: "size"; height: number }
  | { type: "error"; message: string; line: number }
  // What is wrong with it as drawn has all been said.
  | { type: "checked" }
  | { type: "hover" | "picked"; rect: Rect }
  | ({ type: "pick" } & Pick)
  // A key the pane acts on; `held` with ⌘ or Ctrl.
  | { type: "key"; key: string; held?: boolean }
  | { type: "zoom"; by: number; x: number; y: number }
  // A document: how many pages it has, the page it went to by itself, its
  // text as Markdown.
  | { type: "pages"; count: number }
  | { type: "page"; at: number }
  // A document's or a slide's block as it was typed in, with its place
  // when Edit went on, the kind it is to become and what is to follow it;
  // that it was left; and what is selected in the block being edited
  // (nothing, outside one): its tag and its parent's, its marks and its link.
  | {
      type: "edit";
      index: number;
      of?: number;
      before: string;
      around?: [string?, string?];
      html: string;
      as?: string;
      insert?: string;
    }
  | { type: "left" }
  | {
      type: "caret";
      tag?: string;
      parent?: string;
      selected?: boolean;
      bold?: boolean | null;
      italic?: boolean | null;
      href?: string;
    }
  // A sheet's changed tab, its place among how many, as the table its file
  // keeps; whether the sheet can undo and redo; its cells in error.
  | {
      type: "edit";
      tab: number;
      tabs: number;
      table: string;
      all?: boolean;
      undo: boolean;
      redo: boolean;
      errors: string[];
    }
  | { type: "markdown"; text: string }
  // A find's occurrences on each of a document's pages.
  | { type: "find"; counts?: number[] }
  // A sheet's tabs and how many rows a filter hides in the one shown; and its
  // selection: the active cell ("Costs!B2"), what was typed in it and how
  // it shows, the range ("Costs!B2:C6"), what two numbers or more in it
  // come to and its column's rules; and that their dialog is asked for.
  | { type: "sheet"; tabs: string[]; hidden: number; readers?: number }
  | { type: "rules" }
  // What a sheet hands over when asked: itself as a workbook's bytes, its
  // open tab as CSV, or a read file as the text of a sheet to save.
  | { type: "file"; as: string; data: Uint8Array | string }
  // Why a sheet did not make a change that was asked for.
  | { type: "refused"; message: string }
  // That a sheet's engine is calculating, past a moment, and when it is not.
  | { type: "calculating"; on: boolean }
  // The edits that make a sheet's file hold every change asked for, while
  // any is being calculated; bare once none is.
  | {
      type: "held";
      edits?: { tab: number; tabs: number; table: string; all?: boolean }[];
    }
  // A comment on a sheet's selection, asked for from its own menus.
  | { type: "comment" }
  // What is being typed in a sheet's cell, and that text as its file would
  // hold it; `first`, with the table its edit would make, cut around the
  // text, where that can be kept. Bare once it is typed no more.
  | {
      type: "typing";
      text?: string;
      written?: string;
      first?: boolean;
      tab?: number;
      tabs?: number;
      head?: string;
      tail?: string;
      empty?: string;
    }
  | {
      type: "cell";
      ref: string;
      raw: string;
      // What is being typed in it, which goes on.
      typing?: string;
      range: string;
      summary?: { count: number; sum: number; average: number };
      marks?: Marks;
      rules?: Rule[];
      // Whether the selection meets a merged cell.
      merged?: boolean;
    }
  // That merging a sheet's selection empties `count` cells that hold text,
  // which a person is asked about first.
  | { type: "merge"; range: string; count: number }
  // What a sheet's formula may take after `text`: the functions named by
  // its last `typed` letters, or the arguments of the call it stands in,
  // with the one reached.
  | {
      type: "suggest";
      text: string;
      typed?: number;
      options?: { name: string; about: string }[];
      hint?: { name: string; args: string[]; at: number };
    };

// Tells a page what to do: select or edit on or off (the caret put in the
// block at `at`, `over` its words), a mark, a link or another kind for what
// is being edited or a block to follow it, set a tweak, mark the words
// found; a sheet: write into its active cell, do a command, say what a
// formula may take after its text up to the caret. Which part of it shows
// (a page, a tab) is its address's to say: "#3".
export const tell = (
  frame: HTMLIFrameElement | null,
  message:
    | { type: "select" | "edit"; on: boolean; at?: number; over?: boolean }
    | {
        type: "format";
        mark?: "bold" | "italic";
        href?: string;
        as?: string;
        insert?: string;
      }
    | { type: "markdown" }
    | { type: "tweak"; name: string; value: unknown }
    | { type: "find"; text: string; index: number }
    | { type: "input"; value: string }
    | { type: "suggest"; text: string }
    | { type: "do"; command: string; value?: string },
) => frame?.contentWindow?.postMessage({ uagentDesign: 1, ...message }, "*");

// What a sheet's grid takes from the app, as the variables of its stylesheet
// (web/sheet/sheet.css): the palette, a cell's fills and text colours, and
// the type. Resolved here, since the page is another document.
const SHEET_LOOK = {
  ground: "bg",
  ink: "text",
  muted: "muted",
  line: "line",
  edge: "surface",
  head: "raised",
  accent: "accent",
  error: "removed",
  tint: "sheet-tint",
  good: "sheet-good",
  warn: "sheet-warn",
  bad: "sheet-bad",
  "ink-good": "added",
  "ink-warn": "warning",
};
function tellLook(frame: HTMLIFrameElement | null) {
  if (!frame) return;
  const style = getComputedStyle(frame);
  const token = (name: string) => style.getPropertyValue(`--${name}`).trim();
  const set = (name: string, value: string) =>
    tell(frame, { type: "tweak", name: `--sheet-${name}`, value });
  for (const [name, from] of Object.entries(SHEET_LOOK)) set(name, token(from));
  set("font", `${style.fontSize}/${style.lineHeight} ${style.fontFamily}`);
}

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
  // Not live, a frame is a picture of a board (on the canvas, in a legend,
  // as a thumbnail): what it lies on takes the pointer.
  live?: boolean;
  // Only its box, with nothing in it: a long deck's legend runs the few
  // pages near its view. The box stays whatever it holds, so the legend keeps
  // its place as pages come and go.
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
  // What follows "#" names a part of the file (a document's page, a sheet's
  // tab), which the page shows without loading again.
  const file = src.replace(/#.*$/, "");
  // The file whose page has drawn. A page on its way is unseen until it
  // has, and the page before it stays under it, still, where it was.
  const [drawn, setDrawn] = useState<string>();
  const wanted = useLatest(file);
  const left = useRef(src);
  if (drawn === file) left.current = src;
  const paged = !far && !media;
  useLayoutEffect(() => {
    if (!paged) setDrawn(undefined);
  }, [paged]);
  // A sheet takes the app's look as it loads, and again when that changes
  // (the theme, the zoom).
  const [sheet, setSheet] = useState(false);
  // biome-ignore lint/correctness/useExhaustiveDependencies: one listener per frame, before the page can answer
  useLayoutEffect(() => {
    const heard = (event: MessageEvent) => {
      if (event.source !== frame.current?.contentWindow) return;
      if (event.data?.uagentDesign !== 1) return;
      if (event.data.type === "ready") {
        setSheet(!!event.data.sheet);
        if (event.data.sheet) tellLook(frame.current);
      }
      if (event.data.type === "drawn") setDrawn(wanted.current);
      handle.current?.(event.data);
    };
    addEventListener("message", heard);
    return () => removeEventListener("message", heard);
  }, []);
  useEffect(() => {
    if (!sheet) return;
    const restyle = new MutationObserver(() => tellLook(frame.current));
    restyle.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ["data-theme", "style"],
    });
    return () => restyle.disconnect();
  }, [sheet, frame]);
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
      // Nothing of it drawn yet: its outline stands in, a grid for a sheet.
      data-waiting={
        paged && drawn === undefined ? (sheet ? "sheet" : "page") : undefined
      }
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
        [...new Set([drawn ?? file, file])].map((address) => (
          <iframe
            key={address}
            // Only the page wanted is told things and heard.
            ref={
              address === file
                ? (node) => {
                    if (node) frame.current = node;
                  }
                : undefined
            }
            src={address === file ? src : left.current}
            title={address === file ? title : undefined}
            sandbox="allow-scripts allow-forms"
            tabIndex={live ? undefined : -1}
            inert={address !== file}
            data-arriving={address !== drawn || undefined}
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
        ))
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
        live={false}
        src={pageAddress(base, path)}
        title={info?.title || path}
        width={w}
        height={h}
        scale={THUMB_HEIGHT / h}
        onMessage={(message) => message.type === "ready" && setInfo(message)}
      />
    </div>
  );
}
