// What a folder holds is shown as one list of boards, for the canvas a
// design lies on and for the legend a deck or a document is stepped through.
// The host says what each page and its folder is (a listing's `kind`s,
// src/app/workspace_files.cc); a page says the rest once it is drawn.

import type { ArtifactType } from "../../shared/artifact.ts";
import { HTML, IMAGE, SHEET_FILE, VIDEO } from "../../shared/file-types.ts";
import { maxUploadBytes } from "../../shared/limits.ts";
import type { PageInfo, PageMessage } from "../../shared/page-frame.tsx";
import { pageSize } from "../../shared/page-size.ts";
import { bytes } from "../../shared/quantities.ts";

// A document on paper, a sheet of tabs, a deck of slides, a design system,
// or a design: any other pages.
export type Kind = ArtifactType;

export interface Listing {
  // What its pages make the folder: a "deck", a "design", a "system" or "".
  kind?: string;
  // A page says "doc", "sheet", "slide" or "page" when its head declares
  // one; a sheet, how many tabs it has.
  entries: {
    name: string;
    dir: boolean;
    kind?: string;
    size?: number;
    count?: number;
  }[];
}

// One thing drawn: a page of the folder, one page of a document or one tab
// of a sheet; on a design's canvas also a picture, a film, and as a stack
// (its first page, with how many there are) a document, a sheet or a deck
// that lies in the design.
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
  // A document's page: where it starts in the document.
  top?: number;
  // A design's step: its variants and states share a row.
  row?: string;
  // Why it failed, in a few words after its name, else empty: a page
  // threw or is not as it should be drawn (design-helper.js); a picture
  // or a film did not load.
  failed: string;
}

// What a page's frames said, by its path: what it is, what failed in it,
// for a document where print starts each page after the first, and for a
// sheet its tabs.
interface Seen {
  info?: PageInfo;
  errors: string[];
  breaks?: number[];
  tabs?: string[];
}
export type SeenPages = Record<string, Seen>;

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
  const was = all[path] ?? { errors: [] };
  let next: Seen | undefined;
  if (message.type === "ready") next = { ...was, info: message };
  else if (message.type === "size") {
    if (was.info && was.info.height !== message.height)
      next = { ...was, info: { ...was.info, height: message.height } };
  } else if (message.type === "pages") {
    if (String(was.breaks) !== String(message.breaks))
      next = { ...was, breaks: message.breaks };
  } else if (message.type === "sheet") {
    const tabs = strings(message.tabs);
    if (was.tabs?.join("\n") !== tabs.join("\n")) next = { ...was, tabs };
  } else if (message.type === "edit") {
    if ("errors" in message) next = { ...was, errors: strings(message.errors) };
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
    .filter((entry) => entry.kind === "slide")
    .map((entry) => entry.name)
    .sort();

// A deck rearranged by hand, as what the host is asked (routes.fileWrite's
// `moves`): a slide renamed, copied, or with no `to` deleted. The deck's
// order is its names', so a slide's place is the number its name starts
// with.
export interface Move {
  from: string;
  to: string;
  copy?: boolean;
}

// What a slide is called apart from its place.
const rest = (name: string) => name.replace(/^\d+-/, "");

// The renames that make `order` the deck's order: each slide is its place,
// as two digits (three from a hundred slides), then its own name. Only
// those not called so already.
const renames = (order: string[]): Move[] =>
  order
    .map((from, at) => ({
      from,
      to: `${String(at + 1).padStart(order.length < 100 ? 2 : 3, "0")}-${rest(from)}`,
    }))
    .filter((move) => move.from !== move.to);

// The slide at `from` taken to the place `to`.
export function moved(slides: string[], from: number, to: number) {
  const order = [...slides];
  order.splice(to, 0, ...order.splice(from, 1));
  return renames(order);
}

// A copy of the slide at `at` right after it, named for it with `-copy`
// (and a number, while another slide is called so).
export function duplicated(slides: string[], at: number) {
  const stem = rest(slides[at]).replace(/\.html$/, "-copy");
  const taken = new Set(slides.map(rest));
  let copy = `${stem}.html`;
  for (let n = 2; taken.has(copy); n++) copy = `${stem}-${n}.html`;
  const order = [...slides];
  order.splice(at + 1, 0, copy);
  return renames(order).map((move) =>
    move.from === copy ? { from: slides[at], to: move.to, copy: true } : move,
  );
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

// A document and a sheet are each their own, whatever folder they lie in.
export const alone = (kind?: string): kind is "doc" | "sheet" =>
  kind === "doc" || kind === "sheet";

// What the file `name` of a folder is shown as: itself when it is its own;
// any other page is a part of its folder.
export function kindOf(listing: Listing, name: string): Kind {
  const entry = listing.entries.find((entry) => entry.name === name);
  if (alone(entry?.kind)) return entry.kind;
  if (SHEET_FILE.test(name)) return "sheet";
  return listing.kind === "deck" || listing.kind === "system"
    ? listing.kind
    : "design";
}

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
// part of: a document as its pages, a sheet as its tabs, a deck's slide
// among the slides, any other page among the folder's pages in rows, less
// the documents and sheets, which are each their own. With none, its
// canvas: every page in rows, a document or a sheet as a stack, and what
// else lies in the folder: its pictures and films, and a stack for each of
// its `folders` that is a deck.
export function boardsOf(
  dir: string,
  listing: Listing,
  seen: SeenPages,
  name?: string,
  folders: Record<string, Listing> = {},
): Board[] {
  const board = (file: string, like?: PageInfo): Board => {
    const path = join(dir, file);
    const { w, h } = pageSize(seen[path]?.info ?? like);
    return {
      path,
      title: seen[path]?.info?.title || file,
      media: "page",
      w,
      h,
      failed: seen[path]?.errors[0] ?? "",
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
  if (kind === "sheet") {
    // Numbered until the sheet has said what its tabs are called.
    const page = board(name!);
    const { count = 1 } = listing.entries.find((entry) => entry.name === name)!;
    const tabs =
      seen[page.path]?.tabs ??
      Array.from({ length: count }, (_, at) => `${at + 1}`);
    return tabs.map((title) => ({ ...page, title }));
  }
  if (kind === "deck") {
    // A slide not drawn yet is taken to be as large as one that is.
    const slides = slidesOf(listing);
    const like = slides
      .map((file) => seen[join(dir, file)]?.info)
      .find(Boolean);
    return slides.map((file) => board(file, like));
  }
  // What lies on the canvas, by the name that says its row.
  const lying = new Map<string, Board>();
  for (const entry of listing.entries) {
    if (entry.dir) continue;
    const made = board(entry.name);
    const { breaks, tabs } = seen[made.path] ?? {};
    if (HTML.test(entry.name) && !alone(entry.kind))
      lying.set(entry.name, made);
    else if (kind) continue;
    else if (entry.kind === "sheet")
      lying.set(entry.name, {
        ...made,
        media: "stack",
        type: "sheet",
        count: tabs?.length ?? entry.count,
      });
    else if (entry.kind === "doc")
      // Its first page, as tall as print makes it.
      lying.set(entry.name, {
        ...made,
        media: "stack",
        type: "doc",
        count: breaks && breaks.length + 1,
        h: breaks?.[0] ?? made.h,
      });
    else if (IMAGE.test(entry.name) || VIDEO.test(entry.name))
      lying.set(entry.name, {
        ...made,
        media: IMAGE.test(entry.name) ? "image" : "video",
        // The preview address serves no more, so it is not asked for.
        failed:
          (entry.size ?? 0) > maxUploadBytes
            ? `is larger than ${bytes(maxUploadBytes)}`
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

// The boards' failed pages, and what they threw as lines for the agent. A
// picture or a film that did not load says so on the canvas, by its name.
// A sheet opened fails by the cell: each is counted.
export function failures(boards: Board[], seen: SeenPages, kind?: Kind) {
  const paths = [
    ...new Set(
      boards
        .filter((board) => board.failed && !still(board))
        .map((board) => board.path),
    ),
  ];
  const cells = kind === "sheet";
  return {
    count: cells
      ? sum(paths.map((path) => seen[path].errors.length))
      : paths.length,
    text: paths
      .map(
        (path) =>
          `\`${path}\` ${cells ? "has cells in error" : "threw"}: ${seen[path].errors.join("; ")}`,
      )
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
