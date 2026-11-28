// What a folder holds, as one list of boards for the canvas and the legend.
// The host says what each file is shown as (a listing's `kind`s); a page
// says the rest.

import type { ArtifactType } from "../../shared/artifact.ts";
import { HTML, IMAGE, VIDEO } from "../../shared/file-types.ts";
import { maxTransferBytes } from "../../shared/limits.ts";
import type { PageInfo, PageMessage } from "../../shared/page-frame.tsx";
import { pageSize } from "../../shared/page-size.ts";
import { bytes, plural } from "../../shared/quantities.ts";

// A document on paper, a sheet of tabs, a deck of slides, a whiteboard, a
// design system, or a design: any other pages.
export type Kind = ArtifactType;

export interface Listing {
  // What its pages make the folder: a "deck", a "design", a "board", a
  // "system" or "".
  kind?: string;
  // What a page, or an Excel or CSV file, is shown as: itself when it is
  // `alone`, an artifact of its own, else its folder. A sheet says how many
  // tabs it has; a slide, that it is skipped.
  entries: {
    name: string;
    dir: boolean;
    kind?: Kind;
    alone?: boolean;
    size?: number;
    // The size its artboard names, [w, h]; 0 for a height left open.
    artboard?: [number, number];
    count?: number;
    skip?: boolean;
    // The part of a deck a slide opens.
    section?: string;
  }[];
}

// One thing drawn: a page, one page of a document or one tab of a sheet; on
// a design's canvas also a picture, a film, and a document, a sheet or a deck
// as a stack (its first page, with how many there are).
export interface Board {
  // What a click opens: the file, or a stack's first page.
  path: string;
  title: string;
  media: "page" | "image" | "video" | "stack";
  // What a stack is, and how many slides, tabs or (once the document has
  // said them) pages it holds.
  type?: Kind;
  count?: number;
  w: number;
  h: number;
  // The size its file names for it, known before it has drawn.
  named?: { w: number; h: number };
  // Its place among the boards of one file, from 0: a document's page, a
  // sheet's tab. The file's address with "#" and its number shows it.
  at?: number;
  // A design's step: its variants and states share a row.
  row?: string;
  // A slide left out of what is presented and printed.
  skip?: true;
  // The part of a deck it opens.
  section?: string;
  // A page threw, or a picture or film did not load.
  failed: string;
  // What the checker found on a page that did draw.
  issue?: string;
}

// What a page's frames said, by its path: what it is, what failed in it,
// for a document how many pages it has, and for a sheet its tabs.
interface Seen {
  info?: PageInfo;
  errors: string[];
  issues: string[];
  cells: string[];
  pages?: number;
  tabs?: string[];
}
export type SeenPages = Record<string, Seen>;

// A page is untrusted: a document counts as no more pages than this.
const PAGES = 2000;
// What one message may carry of the pages' errors.
const ERRORS_MAX = 1800;

// A page is untrusted: a list it says is taken as short strings, or empty.
const strings = (said: unknown) =>
  Array.isArray(said)
    ? said.slice(0, 100).map((item) => String(item).slice(0, 300))
    : [];

// What is known of the pages once the one at `path` said `message`. A page
// is drawn more than once (on the stage and in the legend), so an error is
// kept once and a repeat changes nothing.
export function hear(
  all: SeenPages,
  path: string,
  message: PageMessage,
): SeenPages {
  const was = all[path] ?? { errors: [], issues: [], cells: [] };
  let next: Seen | undefined;
  if (message.type === "ready") next = { ...was, info: message };
  else if (message.type === "size") {
    if (was.info && was.info.height !== message.height)
      next = { ...was, info: { ...was.info, height: message.height } };
  } else if (message.type === "pages") {
    const pages = Math.min(Math.max(1, Math.floor(message.count) || 1), PAGES);
    if (was.pages !== pages) next = { ...was, pages };
  } else if (message.type === "sheet") {
    const tabs = strings(message.tabs);
    if (was.tabs?.join("\n") !== tabs.join("\n")) next = { ...was, tabs };
  } else if (message.type === "edit") {
    if ("errors" in message) next = { ...was, cells: strings(message.errors) };
  } else if (message.type === "error" || message.type === "issue") {
    const key =
      message.type === "error" ? "errors" : message.cell ? "cells" : "issues";
    const text =
      message.type === "error" && message.line
        ? `${message.message} (line ${message.line})`
        : message.message;
    if (!was[key].includes(text)) next = { ...was, [key]: [...was[key], text] };
  }
  return next ? { ...all, [path]: next } : all;
}

// What is known once `path` was written (none: a file the pages share):
// drawn again, a page says what fails in it now.
export const rewritten = (all: SeenPages, path: string): SeenPages =>
  Object.fromEntries(
    Object.entries(all).map(([at, was]) => [
      at,
      path && at !== path ? was : { ...was, errors: [], issues: [], cells: [] },
    ]),
  );

export const join = (dir: string, name: string) =>
  dir ? `${dir.replace(/\/$/, "")}/${name}` : name;
export const sum = (numbers: number[]) =>
  numbers.reduce((total, number) => total + number, 0);

// The folder's pages: its HTML files, less the sheets, which are no part
// of what the folder is.
export const pagesOf = (listing: Listing) =>
  listing.entries
    .filter(
      (entry) => !entry.dir && HTML.test(entry.name) && entry.kind !== "sheet",
    )
    .map((entry) => entry.name);

// A deck's slides in the order its PDF prints them: by name, as written.
export const slidesOf = (listing: Listing) =>
  listing.entries
    .filter((entry) => entry.kind === "deck")
    .map((entry) => entry.name)
    .sort();

// What the host is asked to rearrange a deck (routes.fileWrite's `moves`): a
// slide renamed, copied, or with no `to` deleted. The deck's order is its
// names', so a slide's place is the number its name starts with.
export interface Move {
  from: string;
  to: string;
  copy?: boolean;
}

// What a slide is called apart from its place.
export const slideName = (name: string) => name.replace(/^\d+-/, "");

// The renames that make `order` the deck's order: each slide is its place,
// as two digits (three from a hundred slides), then its own name. Only
// those not called so already.
const renames = (order: string[]): Move[] =>
  order
    .map((from, at) => ({
      from,
      to: `${String(at + 1).padStart(order.length < 100 ? 2 : 3, "0")}-${slideName(from)}`,
    }))
    .filter((move) => move.from !== move.to);

export function moved(slides: string[], from: number, to: number) {
  const order = [...slides];
  order.splice(to, 0, ...order.splice(from, 1));
  return renames(order);
}

// A slide's name from `stem` that no slide has, apart from its place: with
// a number, while another is called so.
function free(slides: string[], stem: string) {
  const taken = new Set(slides.map(slideName));
  let name = `${stem}.html`;
  for (let n = 2; taken.has(name); n++) name = `${stem}-${n}.html`;
  return name;
}

// A copy of the slide at `at` right after it, named for it with `-copy`.
export function duplicated(slides: string[], at: number) {
  const copy = free(slides, slideName(slides[at]).replace(/\.html$/, "-copy"));
  const order = [...slides];
  order.splice(at + 1, 0, copy);
  return renames(order).map((move) =>
    move.from === copy ? { from: slides[at], to: move.to, copy: true } : move,
  );
}

// A new slide right after the one at `at`: the renames that make room for
// it, and the name of its place, under which it is then made.
export function added(slides: string[], at: number) {
  const fresh = free(slides, "slide");
  const order = [...slides];
  order.splice(at + 1, 0, fresh);
  const moves = renames(order);
  return {
    name: moves.find((move) => move.from === fresh)!.to,
    moves: moves.filter((move) => move.from !== fresh),
  };
}

// The slide to show once `moves` are done, of a deck showing the one at
// `index`: that slide under its new name, or a neighbour when it is the one
// deleted.
export function shownAfter(moves: Move[], slides: string[], index: number) {
  const own = moves.find((move) => move.from === slides[index] && !move.copy);
  return (
    (own ? own.to : slides[index]) ||
    slides[index + 1] ||
    slides[index - 1] ||
    ""
  );
}

// The slide at `at` deleted; the others keep their names.
export const removed = (slides: string[], at: number): Move[] => [
  { from: slides[at], to: "" },
];

// What is known of the pages of `dir` once `moves` are done: a slide is
// known under its new name as it was under its old, so the legend neither
// forgets its title nor redraws it at another size.
export function carried(all: SeenPages, dir: string, moves: Move[]) {
  const next = { ...all };
  for (const move of moves) if (!move.copy) delete next[join(dir, move.from)];
  for (const { from, to } of moves)
    if (to && all[join(dir, from)]) next[join(dir, to)] = all[join(dir, from)];
  return next;
}

// What the host says the file `name` of a folder is shown as; a design
// while it is not listed yet.
export const kindOf = (listing: Listing, name: string): Kind =>
  listing.entries.find((entry) => entry.name === name)?.kind ?? "design";

// A page's step, its leading number (`02-pricing--annual.html` is step
// 02); the variants and states of a step share its row.
const rowOf = (name: string) => /^(\d+)-/.exec(name)?.[1] ?? "";

// A design's files and folders grouped into rows, rows in step order with
// the stepless first, and in a row the pages before what else lies in it,
// each by name.
function rows(names: string[]) {
  const byName = (a: string, b: string) =>
    a.localeCompare(b, undefined, { numeric: true });
  const sorted = [...names].sort(
    (a, b) => +!HTML.test(a) - +!HTML.test(b) || byName(a, b),
  );
  return [...new Set(names.map(rowOf))]
    .sort(byName)
    .map((row) => [row, sorted.filter((name) => rowOf(name) === row)] as const);
}

// The boards of the folder `dir`. With the page `name` opened, what that is
// part of: a document's pages, a sheet's tabs, a deck's slides, a design's
// rows. With none, its canvas: documents, sheets and decks lie on it as stacks.
export function boardsOf(
  dir: string,
  listing: Listing,
  seen: SeenPages,
  name?: string,
  folders: Record<string, Listing> = {},
): Board[] {
  const board = (file: string, like?: PageInfo): Board => {
    const path = join(dir, file);
    const [width, height] =
      listing.entries.find((entry) => entry.name === file)?.artboard ?? [];
    const named = width ? { w: width, h: height || 0 } : undefined;
    const { w, h } = pageSize(
      seen[path]?.info ?? (named ? { artboard: named } : like),
    );
    return {
      path,
      named,
      title: seen[path]?.info?.title || file,
      media: "page",
      w,
      h,
      failed: seen[path]?.errors[0] ?? "",
      issue: [
        seen[path]?.issues.length && "design issue",
        seen[path]?.cells.length && "cell errors",
      ]
        .filter(Boolean)
        .join(", "),
    };
  };
  const kind = name === undefined ? undefined : kindOf(listing, name);
  if (kind === "doc" || kind === "sheet") {
    // One page until the document has said how many; a sheet's tabs are
    // numbered until it has said what they are called.
    const page = board(name!);
    const { pages = 1, tabs } = seen[page.path] ?? {};
    const { count = 1 } = listing.entries.find((entry) => entry.name === name)!;
    const titles =
      kind === "doc"
        ? Array.from({ length: pages }, () => page.title)
        : (tabs ?? Array.from({ length: count }, (_, at) => `${at + 1}`));
    return titles.map((title, at) => ({ ...page, title, at }));
  }
  if (kind === "deck") {
    // A slide with no declared size borrows one from a loaded neighbour.
    const slides = slidesOf(listing);
    const like = slides
      .map((file) => seen[join(dir, file)]?.info)
      .find(Boolean);
    const entry = (file: string) =>
      listing.entries.find((entry) => entry.name === file);
    return slides.map((file) => {
      const { skip, section } = entry(file) ?? {};
      return {
        ...board(file, like),
        ...(skip ? { skip: true as const } : {}),
        ...(section ? { section } : {}),
      };
    });
  }
  // What lies on the canvas, by the name that says its row.
  const lying = new Map<string, Board>();
  for (const entry of listing.entries) {
    if (entry.dir) continue;
    const made = board(entry.name);
    const { pages, tabs } = seen[made.path] ?? {};
    if (HTML.test(entry.name) && !entry.alone) lying.set(entry.name, made);
    else if (kind) continue;
    // An Excel or CSV file is data beside the design, not drawn on it.
    else if (entry.kind === "sheet" && HTML.test(entry.name))
      lying.set(entry.name, {
        ...made,
        media: "stack",
        type: "sheet",
        count: tabs?.length ?? entry.count,
      });
    else if (entry.kind === "doc")
      lying.set(entry.name, {
        ...made,
        media: "stack",
        type: "doc",
        count: pages,
      });
    else if (IMAGE.test(entry.name) || VIDEO.test(entry.name))
      lying.set(entry.name, {
        ...made,
        media: IMAGE.test(entry.name) ? "image" : "video",
        // The preview address serves no more, so it is not asked for.
        failed:
          (entry.size ?? 0) > maxTransferBytes
            ? `is larger than ${bytes(maxTransferBytes)}`
            : made.failed,
      });
  }
  for (const [folder, inner] of Object.entries(kind ? {} : folders)) {
    const slides = slidesOf(inner);
    if (inner.kind === "deck")
      lying.set(folder, {
        ...board(join(folder, slides[0])),
        title: folder,
        media: "stack",
        type: "deck",
        count: slides.length,
      });
  }
  return rows([...lying.keys()]).flatMap(([row, names]) =>
    names.map((name) => ({ ...lying.get(name)!, row })),
  );
}

// A picture or a film: shown as it is, not run.
export const still = (board: Board) =>
  board.media === "image" || board.media === "video";

// Whether a board is as large as it will be drawn: it has said its size (a
// document's stack its pages too), or it is a picture or a film that will
// not load.
export const settled = (board: Board, seen: SeenPages) =>
  seen[board.path]?.info
    ? board.media !== "stack" || !!board.count
    : still(board) && !!board.failed;

// Frames of the same file report the same diagnostics. Count files for
// runtime and design checks, and individual cells for formula errors.
export function boardDiagnostics(boards: Board[], seen: SeenPages) {
  const paths = new Set(
    boards
      .filter((board) => !still(board) && (board.failed || board.issue))
      .map((board) => board.path),
  );
  let failed = 0,
    flagged = 0,
    cells = 0;
  const lines: string[] = [];
  for (const path of paths) {
    const page = seen[path];
    if (!page) continue;
    const add = (what: string, details: string[]) => {
      if (details.length)
        lines.push(`\`${path}\` ${what}: ${details.join("; ")}`);
    };
    add("threw", page.errors);
    add("has design issues", page.issues);
    add("has cells in error", page.cells);
    if (page.errors.length) failed++;
    if (page.issues.length) flagged++;
    cells += page.cells.length;
  }
  return {
    summary: [
      failed && `${plural(failed, "page")} failed to run`,
      flagged && `${plural(flagged, "page")} with design issues`,
      cells && `${plural(cells, "cell")} in error`,
    ]
      .filter(Boolean)
      .join(" · "),
    text: lines.join("\n").slice(0, ERRORS_MAX),
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
