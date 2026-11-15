// Where the Files pane is: exactly one view, and the views Back returns to.
// A view's data is looked up by the path it names, never "the current one".

import type { FileTarget } from "../../shared/types.ts";
import { HTML, MARKDOWN, nameOf, paged } from "../../shared/file-types.ts";
import {
  alone,
  join,
  kindOf,
  type Listing,
  pagesOf,
  slidesOf,
} from "./boards.ts";

export type Lines = [number, number];
export type View =
  // A folder's tree; with `note`, why the pane landed here.
  | { kind: "list"; dir: string; note?: string }
  | {
      kind: "file";
      path: string;
      lines?: Lines;
      source?: boolean;
      diff?: boolean;
    }
  // A page across the pane, with the legend of what it is part of (a deck's
  // slides, a document's pages, a design's boards).
  | { kind: "sequence"; path: string }
  // A design's pages on a plane.
  | { kind: "canvas"; dir: string }
  // Several files share a cited name: the person picks one.
  | { kind: "pick"; name: string; paths: string[]; lines?: Lines }
  // A path being looked up; `fresh` when it came from outside the pane.
  | {
      kind: "resolve";
      path: string;
      lines?: Lines;
      diff?: boolean;
      fresh: boolean;
    }
  | { kind: "outside"; path: string };
type ResolveView = Extract<View, { kind: "resolve" }>;

export interface Flow {
  view: View;
  back: View[];
}

export const START: Flow = { view: { kind: "list", dir: "" }, back: [] };

export const dirOf = (path: string) =>
  path.slice(0, Math.max(0, path.lastIndexOf("/")));

export const push = (flow: Flow, view: View): Flow => ({
  view,
  back: [...flow.back, flow.view],
});
export const swap = (flow: Flow, view: View): Flow => ({ ...flow, view });
export const pop = (flow: Flow): Flow =>
  flow.back.length
    ? { view: flow.back[flow.back.length - 1], back: flow.back.slice(0, -1) }
    : flow;

// "12" or "12-20" as the first and last line.
export function lineRange(lines?: string): Lines | undefined {
  if (!lines) return undefined;
  const [first, last = first] = lines.split("-").map(Number);
  return [Math.min(first, last), Math.max(first, last)];
}

// A path a message cited, in the conversation's `folder`: where the pane
// goes, with a fresh history.
export function cite(target: FileTarget, folder: string): Flow {
  const written = target.path;
  const path = written.startsWith("/")
    ? written.startsWith(`${folder}/`)
      ? written.slice(folder.length + 1)
      : undefined
    : written.startsWith("~")
      ? undefined
      : written.replace(/^(\.\/)+/, "");
  if (path === undefined)
    return { view: { kind: "outside", path: written }, back: [] };
  return {
    view: {
      kind: "resolve",
      path,
      lines: lineRange(target.lines),
      diff: !!target.changes,
      fresh: true,
    },
    back: [],
  };
}

// What a path that is there shows: a page in its sequence, else the file.
// Rendered pages and Markdown have no lines to mark: cited at lines, they
// open as their source.
export const opened = (path: string, lines?: Lines): View =>
  paged(path) && !lines
    ? { kind: "sequence", path }
    : {
        kind: "file",
        path,
        lines,
        source: !!lines && (MARKDOWN.test(path) || HTML.test(path)),
      };

// A view opened from the list itself (the tree, search, the changes): Back
// is the list of its folder.
export const fromList = (view: View): Flow => ({
  view,
  back: [{ kind: "list", dir: folderOfView(view) }],
});

// A path clicked inside the pane: looked up in place, Back to here.
export const follow = (flow: Flow, path: string, lines?: Lines) =>
  push(flow, { kind: "resolve", path, lines, fresh: false });

// What a lookup reads: a folder's entries and what they are (null when
// there is no such folder), and the folder's paths whose name matches.
interface Lookup {
  list: (dir: string) => Promise<Listing | null>;
  find: (name: string) => Promise<string[]>;
}

// What a folder opens as when it is more than its list: a deck at its first
// slide, a design system on its brand book, two pages or more on their
// canvas, one page as itself. Its sheets do not count: each opens as itself.
export function folderView(dir: string, listing: Listing): View | undefined {
  const pages = pagesOf(listing);
  const at = (name: string): View => ({
    kind: "sequence",
    path: join(dir, name),
  });
  if (listing.kind === "deck") return at(slidesOf(listing)[0]);
  if (listing.kind === "system" && pages.includes("book.html"))
    return at("book.html");
  if (pages.length > 1) return { kind: "canvas", dir };
  return pages.length ? at(pages[0]) : undefined;
}

// The view a path that is there opens, and the folder's views above it.
function landing(
  path: string,
  listing: Listing,
  lines?: Lines,
  diff?: boolean,
): { view: View; up: View[] } {
  const dir = dirOf(path);
  const list: View = { kind: "list", dir };
  if (diff) return { view: { kind: "file", path, diff: true }, up: [list] };
  // A design's page lies on its canvas; a document, a sheet and a deck's
  // slide have none.
  const view = opened(path, lines);
  const board =
    view.kind === "sequence" &&
    !alone(kindOf(listing, nameOf(path))) &&
    folderView(dir, listing)?.kind === "canvas";
  return { view, up: board ? [list, { kind: "canvas", dir }] : [list] };
}

// Looks a path up and settles the flow on what it is: what a folder opens
// as, or its list; a page or a file; a pick among several files named so;
// or the top with a note when it is nowhere.
export async function resolve(
  flow: Flow,
  target: ResolveView,
  io: Lookup,
): Promise<Flow> {
  const settle = (view: View, up: View[]): Flow =>
    target.fresh ? { view, back: up } : swap(flow, view);
  const missing = (what: string) =>
    settle(
      { kind: "list", dir: "", note: `${what} is not in this folder.` },
      [],
    );
  if (target.path.endsWith("/")) {
    const dir = target.path.replace(/\/+$/, "");
    const listing = await io.list(dir);
    if (!listing) return missing(target.path);
    const list: View = { kind: "list", dir };
    const view = folderView(dir, listing);
    return view ? settle(view, [list]) : settle(list, []);
  }
  const listing = await io.list(dirOf(target.path));
  const name = nameOf(target.path);
  if (listing?.entries.some((entry) => !entry.dir && entry.name === name)) {
    const { view, up } = landing(
      target.path,
      listing,
      target.lines,
      target.diff,
    );
    return settle(view, up);
  }
  // Not there as written: a name the folder holds somewhere else.
  const named = (await io.find(name)).filter(
    (path) => path === target.path || path.endsWith(`/${target.path}`),
  );
  if (named.length === 1)
    return resolve(flow, { ...target, path: named[0] }, io);
  if (named.length > 1)
    return settle(
      { kind: "pick", name: target.path, paths: named, lines: target.lines },
      [],
    );
  return missing(target.path);
}

// The folder whose listing a view shows or sits in.
export function folderOfView(view: View) {
  switch (view.kind) {
    case "list":
    case "canvas":
      return view.dir;
    case "file":
    case "sequence":
      return dirOf(view.path);
    default:
      return "";
  }
}

export function backLabel(view: View) {
  switch (view.kind) {
    case "canvas":
      return "Back to the canvas";
    case "sequence":
    case "file":
      return `Back to ${nameOf(view.path)}`;
    case "pick":
      return "Back to the matches";
    default:
      return "Back to the list";
  }
}
