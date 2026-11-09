// Where the Files pane is: exactly one view, and the views Back returns to.
// Every way in is one step: a path from outside the pane (a reply, a card,
// the Library) starts a fresh history that leads up to its own folder; a
// click inside the pane pushes; stepping through slides replaces the page;
// Back pops. Data for a view is looked up by the path it names, never "the
// current one". The design and its route table: the "Files flow" page of
// the docs and decks canvas.

import type { FileTarget } from "../../shared/types.ts";
import { HTML, MARKDOWN, nameOf } from "../../shared/file-types.ts";

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
  // One page across the pane; `present` starts presenting once.
  | { kind: "page"; path: string; present?: boolean }
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
export type ResolveView = Extract<View, { kind: "resolve" }>;

export interface Flow {
  view: View;
  back: View[];
}

export const START: Flow = { view: { kind: "list", dir: "" }, back: [] };

export const dirOf = (path: string) =>
  path.slice(0, Math.max(0, path.lastIndexOf("/")));
const join = (dir: string, name: string) => (dir ? `${dir}/${name}` : name);

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

// A path clicked inside the pane: looked up in place, Back to here.
export const follow = (flow: Flow, path: string, lines?: Lines) =>
  push(flow, { kind: "resolve", path, lines, fresh: false });

interface Listing {
  entries: { name: string; dir: boolean }[];
}
// What a lookup reads: a folder's entries (null when there is no such
// folder) and the folder's paths whose name matches.
export interface Lookup {
  list: (dir: string) => Promise<Listing | null>;
  find: (name: string) => Promise<string[]>;
}

const pagesOf = (listing: Listing) =>
  listing.entries
    .filter((entry) => !entry.dir && HTML.test(entry.name))
    .map((entry) => entry.name);

// The view a path that is there opens, and the folder's views above it.
function landing(
  path: string,
  pages: string[],
  lines?: Lines,
  diff?: boolean,
): { view: View; up: View[] } {
  const dir = dirOf(path);
  const list: View = { kind: "list", dir };
  if (diff) return { view: { kind: "file", path, diff: true }, up: [list] };
  // Rendered pages and Markdown have no lines to mark: cited at lines, they
  // open as their source.
  if (HTML.test(path) && !lines)
    return {
      view: { kind: "page", path },
      up: pages.length > 1 ? [list, { kind: "canvas", dir }] : [list],
    };
  return {
    view: {
      kind: "file",
      path,
      lines,
      source: !!lines && (MARKDOWN.test(path) || HTML.test(path)),
    },
    up: [list],
  };
}

// Looks a path up and settles the flow on what it is: a folder's canvas,
// its one page or its list; a page or a file; a pick among several files
// named so; or the top with a note when it is nowhere.
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
    const pages = pagesOf(listing);
    const list: View = { kind: "list", dir };
    if (pages.length > 1) return settle({ kind: "canvas", dir }, [list]);
    if (pages.length === 1)
      return settle({ kind: "page", path: join(dir, pages[0]) }, [list]);
    return settle(list, []);
  }
  const listing = await io.list(dirOf(target.path));
  const name = nameOf(target.path);
  if (listing?.entries.some((entry) => !entry.dir && entry.name === name)) {
    const { view, up } = landing(
      target.path,
      pagesOf(listing),
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
    case "page":
      return dirOf(view.path);
    default:
      return "";
  }
}

// What Back says it returns to.
export function backLabel(view: View) {
  switch (view.kind) {
    case "canvas":
      return "Back to the canvas";
    case "page":
      return `Back to ${nameOf(view.path)}`;
    case "file":
      return `Back to ${nameOf(view.path)}`;
    case "pick":
      return "Back to the matches";
    default:
      return "Back to the list";
  }
}
