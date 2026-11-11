// What a folder's pages are shown as: one list of boards, for the canvas a
// design lies on and for the legend a deck or a document is stepped through.
// The host says what each page and its folder is (a listing's `kind`s,
// src/app/workspace_files.cc); a page says the rest once it is drawn.

import { HTML } from "../../shared/file-types.ts";
import type { PageInfo, PageMessage } from "../../shared/page-frame.tsx";
import { pageSize } from "../../shared/page-size.ts";

// A document on paper, a deck of slides, a design system, or a design: any
// other pages.
export type Kind = "doc" | "deck" | "system" | "design";

export interface Listing {
  // What its pages make the folder: a "deck", a "design", a "system" or "".
  kind?: string;
  // A page says "doc", "slide" or "page" when its head declares one.
  entries: { name: string; dir: boolean; kind?: string }[];
}

// One thing drawn: a page of the folder, or one page of a document.
export interface Board {
  path: string;
  title: string;
  w: number;
  h: number;
  // A document's page: where it starts in the document.
  top?: number;
  // A design's step: its variants and states share a row.
  row?: string;
  // It threw, or is not as it should be drawn (design-helper.js).
  failed: boolean;
}

// What a page's frames said, by its path: what it is, what failed in it
// and, for a document, where print starts each page after the first.
interface Seen {
  info?: PageInfo;
  errors: string[];
  breaks?: number[];
}
export type SeenPages = Record<string, Seen>;

// What one message may carry of the pages' errors.
const ERRORS_MAX = 1800;

// What is known of the pages once the one at `path` said `message`. A page
// is drawn more than once (on the stage and in the legend), so an error is
// kept once and a repeat changes nothing.
export function hear(
  all: SeenPages,
  path: string,
  message: PageMessage,
): SeenPages {
  const was = all[path] ?? { errors: [] };
  let next: Seen | undefined;
  if (message.type === "ready") next = { ...was, info: message };
  else if (message.type === "size") {
    if (was.info && was.info.height !== message.height)
      next = { ...was, info: { ...was.info, height: message.height } };
  } else if (message.type === "pages") {
    if (String(was.breaks) !== String(message.breaks))
      next = { ...was, breaks: message.breaks };
  } else if (message.type === "error") {
    const error = `${message.message}${message.line ? ` (line ${message.line})` : ""}`;
    if (!was.errors.includes(error))
      next = { ...was, errors: [...was.errors, error] };
  }
  return next ? { ...all, [path]: next } : all;
}

// What is known once `path` was written (none: a file the pages share):
// drawn again, a page says what fails in it now.
export const rewritten = (all: SeenPages, path: string): SeenPages =>
  Object.fromEntries(
    Object.entries(all).map(([at, was]) => [
      at,
      path && at !== path ? was : { ...was, errors: [] },
    ]),
  );

export const join = (dir: string, name: string) =>
  dir ? `${dir.replace(/\/$/, "")}/${name}` : name;
export const sum = (numbers: number[]) =>
  numbers.reduce((total, number) => total + number, 0);

// The folder's HTML files.
export const pagesOf = (listing: Listing) =>
  listing.entries
    .filter((entry) => !entry.dir && HTML.test(entry.name))
    .map((entry) => entry.name);

// A deck's slides in the order its PDF prints them: by name, as written.
export const slidesOf = (listing: Listing) =>
  listing.entries
    .filter((entry) => entry.kind === "slide")
    .map((entry) => entry.name)
    .sort();

// What the file `name` of a folder is shown as: a document is its own
// whatever folder it lies in; any other page is a part of its folder.
export function kindOf(listing: Listing, name: string): Kind {
  const entry = listing.entries.find((entry) => entry.name === name);
  if (entry?.kind === "doc") return "doc";
  return listing.kind === "deck" || listing.kind === "system"
    ? listing.kind
    : "design";
}

// A page's step, its leading number (`02-pricing--annual.html` is step
// 02); the variants and states of a step share its row.
const rowOf = (name: string) => /^(\d+)-/.exec(name)?.[1] ?? "";

// A design's files grouped into rows, rows and files in name order: the
// pages without a step first.
function rows(names: string[]) {
  const sorted = [...names].sort((a, b) =>
    a.localeCompare(b, undefined, { numeric: true }),
  );
  const groups = new Map<string, string[]>();
  for (const name of sorted.filter((name) => !rowOf(name)))
    groups.set("", [...(groups.get("") ?? []), name]);
  for (const name of sorted.filter(rowOf))
    groups.set(rowOf(name), [...(groups.get(rowOf(name)) ?? []), name]);
  return [...groups];
}

// The boards of the folder `dir`. On its canvas, every page in rows. With
// the page `name` opened, what that is part of: a document as its pages, a
// deck's slide among the slides, any other page among the folder's pages in
// rows, less the documents, which are each their own.
export function boardsOf(
  dir: string,
  listing: Listing,
  seen: SeenPages,
  name?: string,
): Board[] {
  const board = (file: string, like?: PageInfo): Board => {
    const path = join(dir, file);
    const { w, h } = pageSize(seen[path]?.info ?? like);
    return {
      path,
      title: seen[path]?.info?.title || file,
      w,
      h,
      failed: !!seen[path]?.errors.length,
    };
  };
  const kind = name === undefined ? undefined : kindOf(listing, name);
  if (kind === "doc") {
    const page = board(name!);
    const { info, breaks = [] } = seen[page.path] ?? {};
    const tops = [0, ...breaks];
    return tops.map((top, at) => ({
      ...page,
      top,
      h: (tops[at + 1] ?? Math.max(top, info?.height ?? 0)) - top,
    }));
  }
  if (kind === "deck") {
    // A slide not drawn yet is taken to be as large as one that is.
    const slides = slidesOf(listing);
    const like = slides
      .map((file) => seen[join(dir, file)]?.info)
      .find(Boolean);
    return slides.map((file) => board(file, like));
  }
  const pages = pagesOf(
    kind
      ? { entries: listing.entries.filter((entry) => entry.kind !== "doc") }
      : listing,
  );
  return rows(pages).flatMap(([row, files]) =>
    files.map((file) => ({ ...board(file), row })),
  );
}

// The document's page at a scroll offset: the last one that starts at or
// above it, and the last of all once the document is scrolled to its end
// (a short last page never reaches the top).
export function pageAt(boards: Board[], top: number, end: boolean) {
  if (end && top > 0) return boards.length - 1;
  return Math.max(
    0,
    boards.findLastIndex((board) => (board.top ?? 0) <= top + 1),
  );
}

// The boards' failed pages, and what they threw as lines for the agent.
export function failures(boards: Board[], seen: SeenPages) {
  const paths = [
    ...new Set(
      boards.filter((board) => board.failed).map((board) => board.path),
    ),
  ];
  return {
    count: paths.length,
    text: paths
      .map((path) => `\`${path}\` threw: ${seen[path].errors.join("; ")}`)
      .join("\n")
      .slice(0, ERRORS_MAX),
  };
}

// Which board holds the `spot`th of the occurrences counted per board, and
// which of that board's it is.
export function spotAt(counts: number[], spot: number) {
  let at = 0;
  let nth = spot;
  while (at < counts.length - 1 && nth >= counts[at]) nth -= counts[at++];
  return { at, nth };
}
