import type { ComponentChildren } from "preact";
import {
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import {
  Check,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  ExternalLink,
  FileOutput,
  MessageSquare,
  LayoutGrid,
  PanelLeft,
  PenLine,
  MousePointerClick,
  Pencil,
  Presentation,
  Redo2,
  Search,
  SlidersHorizontal,
  Undo2,
} from "lucide-preact";
import {
  Bar,
  Actions,
  Button,
  ConfirmModal,
  IconButton,
  Notice,
  Spinner,
} from "../../shared/ui.tsx";
import { plural } from "../../shared/quantities.ts";
import { Textarea } from "../../shared/form-controls.tsx";
import Frame, {
  type PageInfo,
  type PageMessage,
  type Pick,
  type Rect,
  tell,
} from "../../shared/page-frame.tsx";
import { pageSize } from "../../shared/page-size.ts";
import { FindField } from "../../shared/find-field.tsx";
import { occurrences } from "../../../sheet/text.js";
import {
  added,
  type Board,
  type Kind,
  type Move,
  moved,
  shownAfter,
  spotAt,
  sum,
} from "./boards.ts";
import Legend from "./legend.tsx";
import { FormatBar, FormulaBar, SheetBar } from "./sheet.tsx";
import {
  type Cell,
  cellOf,
  type Draft,
  draftOf,
  editOf,
  fileOf,
  heldOf,
  type Offer,
  offerOf,
  type TableEdit,
} from "./sheet-edit.ts";
import { columnOf, RulesDialog } from "./sheet-rules.tsx";
import SlideActions from "./slide-actions.tsx";
import {
  type Caret,
  caretOf,
  DOC_INSERTS,
  type Format,
  LinkField,
  TextTools,
} from "./format.tsx";
import { InsertBar, PickBar } from "../../shared/float-bars.tsx";
import Tweaks, { type Tweak } from "./tweaks.tsx";
import { dirOf } from "./flow.ts";
import { readTokens } from "./tokens.ts";
import { bindZoomWheel } from "../../shared/zoom.ts";
import { useLatest } from "../../shared/use-latest.ts";
import { coarsePointer, useRoom } from "../../shared/layout.ts";
import { ZoomControls } from "../../shared/zoom-surface.tsx";
import type { ViewedImage } from "../../shared/attachments.tsx";
import { STEPS, usePresentChannel } from "./present.ts";
import {
  type BlockEdit,
  blankSlide,
  notesEdit,
  type Saved,
  markSlide,
  UNSPLICED,
} from "./doc-edit.ts";
import {
  BOARD,
  type Chosen,
  chosenOf,
  ItemTools,
  SLIDE,
} from "./item-tools.tsx";
import { downloadBlob } from "../../shared/share.ts";
import { copyDrawn, type Drawn, saveDrawn } from "./drawn.ts";
import { Menu, MenuItem, MenuSub } from "../../shared/menu.tsx";
import { nameOf, SHEET_FILE } from "../../shared/file-types.ts";
import { useResource } from "../../shared/use-resource.ts";

// Why a change by hand was not written.
const REFUSED =
  "That change was not made: the file does not take it as it stands. It is shown as the file is now.";
// False: the file cannot take it by itself (doc-edit.ts); else it does not
// take it as it stands.
const refuse = (made?: false | null): never => {
  throw new Error(made === false ? `Not saved: ${UNSPLICED}` : REFUSED);
};
// Said in the tools' row while a sheet's engine takes more than a moment.
const CALCULATING = "Calculating…";
// What a sheet says of itself, before it has.
const UNTOLD = { hidden: 0, undo: false, redo: false, readers: 0 };
// Where the pane goes with a tab put in, copied or moved: the page follows.
const TAB_AFTER: Record<string, (index: number, tabs: number) => number> = {
  "tab-add": (_, tabs) => tabs,
  "tab-copy": (index) => index + 1,
  "tab-right": (index) => index + 1,
  "tab-left": (index) => index - 1,
};

// How soon a change of the file held for the person is offered again.
const RENEW_MS = 500;
// The zoom of an opened page, as a scale of its real size, and one step.
const ZOOM_MIN = 0.1;
const ZOOM_MAX = 4;
const ZOOM_STEP = 1.25;

// A page opened across the pane with what it is part of beside it: a deck's
// slides, a document's pages, a design's boards. A sheet is its grid, which
// keeps the keys and scrolls in its frame (sheet.tsx).

// A number a page said, which is untrusted: whole, and none below zero.
const natural = (said: unknown) =>
  Math.min(Math.max(0, Math.floor(Number(said)) || 0), 1e6);
// The lines it adds to the draft, which the artifact skill reads.
const commentLine = (path: string, where: string, comment: string) =>
  `\`${path}\` ${where} — ${comment}`;
// A picked element as those lines name it, and as the pane does.
const named = (pick: Pick, name: string) =>
  `${name}${pick.text ? ` “${pick.text}”` : ""}`;

// What a page's source draws as text, a text node each, as the page's own
// find walks them: no script or style, and not a slide's notes.
function drawnText(html: string, slide: boolean) {
  const page = new DOMParser().parseFromString(html, "text/html");
  for (const unseen of page.querySelectorAll("script, style, template"))
    unseen.remove();
  if (slide) [...page.querySelectorAll("body > aside")].pop()?.remove();
  const walker = page.createTreeWalker(page.body, NodeFilter.SHOW_TEXT);
  const texts: string[] = [];
  while (walker.nextNode()) texts.push(walker.currentNode.textContent || "");
  return texts;
}

export default function Sequence({
  path,
  kind,
  boards,
  page,
  heard,
  go,
  addToChat,
  base,
  read,
  drawn,
  presenter,
  save,
  uneditable,
  settle,
  forget,
  unwritten,
  fresh,
  writing,
  typing,
  saveAs,
  arrange,
  cite,
  rewrite,
  undo,
  undoable,
  arranging,
  view,
}: {
  path: string;
  // What the page is part of, and that as boards in order: a deck's slides,
  // a document's pages, a design's pages.
  kind: Kind;
  boards: Board[];
  // A file's preview address, with its version.
  page: (path: string) => string;
  // What a page of the folder said, for whoever keeps it.
  heard: (path: string) => (message: PageMessage) => void;
  // The folder's preview address, and a file of the folder as text: a
  // design system the page links shows under Theme.
  base: string;
  read: (path: string) => Promise<string>;
  // The address the agent's browser draws a path at as a PNG or a PDF (a
  // folder's PDF is its deck), where it has one.
  drawn?: Drawn;
  presenter?: (slides: string[], at: string) => void;
  // A document's or a slide's edited block, or a sheet's edited tab, saved
  // to its file. A block's answers with the place of the block the caret
  // then belongs in.
  save?: (edit: BlockEdit | TableEdit) => Promise<unknown>;
  // Why the file's text cannot be edited here, when it cannot.
  uneditable?: string;
  // Resolves once what was typed in a block is in its file; and what is
  // dropped of it when its page is drawn again from the file.
  settle: () => Promise<unknown>;
  forget: () => void;
  // Whether what was typed in a block is not all in its file yet.
  unwritten: () => boolean;
  // Counts the changes someone else made to the file, which the page takes
  // in place where it can; and whether a call is writing it now.
  fresh: number;
  writing?: boolean;
  // A sheet's cell being typed in and the changes it is calculating, as the
  // edits they would make: written if the sheet is left first. None once
  // there are none.
  typing: (edit?: () => TableEdit[], calculating?: boolean) => void;
  // An Excel or CSV file's tabs, as a sheet's text, saved beside the file.
  saveAs?: (text: string) => Promise<void>;
  // A deck's slides renamed, copied or deleted as `moves` say, after which
  // the slide named `show` is the one shown; `arranging` while that runs.
  // With `add`, a slide is made first, for the moves to place.
  arrange?: (
    moves: Move[],
    show: string,
    add?: { name: string; text: string },
  ) => void;
  // A file of the folder opened at a line, from the page.
  cite?: (path: string, line: number) => void;
  // A slide's file written as `change` makes it of what it holds.
  rewrite?: (
    path: string,
    change: (source: string) => string,
  ) => Promise<unknown>;
  // The last change made to its file by hand undone, or `redo` made again;
  // whether it was, and whether either can be.
  undo?: (redo: boolean) => Promise<boolean>;
  undoable?: { undo: boolean; redo: boolean };
  arranging?: boolean;
  // Opens a picture in the image viewer, which can draw on it.
  view: (image: ViewedImage) => void;
  go: (path: string) => void;
  addToChat?: (text: string, keep?: boolean) => void;
}) {
  const doc = kind === "doc";
  const sheet = kind === "sheet";
  const unit = kind === "deck" ? "Slide" : "Page";
  const src = page(path);
  // A board's address: its file's, with its version, and after "#" its
  // number among the file's boards (the first too: an address that loses
  // its "#" loads again). Those of all, and as one value.
  const address = (board: Board, more = "") =>
    `${page(board.path)}${more}${board.at === undefined ? "" : `#${board.at + 1}`}`;
  // A block saved from here is in the file, which the legend draws again
  // once the block or the page is left: in WebKit a frame that loads ends
  // what the block can undo.
  const wrote = useRef(0);
  const [saves, setSaves] = useState(0);
  const srcs = boards.map((board) =>
    address(board, saves ? `&saved=${saves}` : ""),
  );
  const stamp = srcs.join("\n");
  const frame = useRef<HTMLIFrameElement>(null);
  const stage = useRef<HTMLDivElement>(null);
  const scroller = useRef<HTMLDivElement>(null);
  const room = useRoom(scroller);
  // What the page said it is. It counts once the page has drawn: until then
  // the stage and its tools are those of the page before it.
  const [said, setSaid] = useState<PageInfo & { drawn?: true }>();
  const staged = useRef<PageInfo>(undefined);
  if (said?.drawn) staged.current = said;
  const info = staged.current;
  // What Edit changes in place: a document's or a slide's text, a slide's
  // elements, a whiteboard's items.
  const board = !!info?.board;
  const editable = doc || kind === "deck" || board;
  // Select picks an element to comment on; Edit lets a document's or a
  // slide's text be typed in place. One at a time, told to the page as each
  // goes on or off.
  const [mode, setMode] = useState<"" | "select" | "edit">("");
  // Where Edit cannot be offered, it is off before the page there can be
  // told of it.
  if (uneditable && mode === "edit") setMode("");
  const enter = (next: typeof mode) => {
    if (mode) tell(frame.current, { type: mode, on: false });
    if (next) tell(frame.current, { type: next, on: true });
    setMode(next);
    setHover(undefined);
    setCaret(undefined);
    setLinking(false);
    setTool("");
    setChosen(undefined);
    if (next !== "select") setPicked(undefined);
  };
  // What is selected in the block being edited, and whether its link's
  // address is being typed.
  const [caret, setCaret] = useState<Caret>();
  const [linking, setLinking] = useState(false);
  // The block the caret goes to once the page is drawn again: to its end,
  // or `over` the words of a new one.
  const caretAt = useRef<{ at: number; over: boolean }>(undefined);
  const [hover, setHover] = useState<Rect>();
  // A value's source, drawn under it; kept while the pointer is on its chip.
  const [source, setSource] = useState<{
    rect: Rect;
    label: string;
    path: string;
    line: number;
  }>();
  const holding = useRef(false);
  const unsourced = useRef<ReturnType<typeof setTimeout>>(undefined);
  const [picked, setPicked] = useState<Pick>();
  // While Edit is on a whiteboard's items, or a slide's, are changed by
  // hand: a tool armed for the next press, and what is picked.
  const hand =
    mode !== "edit"
      ? undefined
      : board
        ? BOARD
        : kind === "deck"
          ? SLIDE
          : undefined;
  const [tool, setTool] = useState("");
  const [chosen, setChosen] = useState<Chosen>();
  const arm = (next: string) => {
    setTool(next);
    tell(frame.current, { type: "arm", tool: next, made: hand?.made?.[next] });
  };
  const [comment, setComment] = useState("");
  const [tweaking, setTweaking] = useState(false);
  // Reset reloads the page: what it declares comes back.
  const [load, setLoad] = useState(0);
  // The address whose page has drawn. What is asked of a page before it has
  // is lost, so the controls that ask are disabled until the one wanted has.
  const [arrived, setArrived] = useState("");
  const wanted = load ? `${src}&load=${load}` : src;
  const live = arrived === wanted;
  // The file changed by someone else: the page takes it as it stands, its
  // zoom, scroll and selection kept, or loads again where it cannot. Not
  // while what was typed is on its way to the file, or the page is not live.
  const again = useRef<ReturnType<typeof setTimeout>>(undefined);
  const renew = useLatest(() => {
    clearTimeout(again.current);
    if (!live || unwritten())
      again.current = setTimeout(() => renew.current(), RENEW_MS);
    else
      void read(path).then(
        (html) => tell(frame.current, { type: "update", html }),
        () => setLoad((count) => count + 1),
      );
  });
  const heardFresh = useRef(fresh);
  useEffect(() => {
    if (fresh === heardFresh.current) return;
    heardFresh.current = fresh;
    renew.current();
  }, [fresh, renew]);
  useEffect(() => () => clearTimeout(again.current), []);
  // Another page: nothing picked, typed or zoomed carries over.
  useEffect(() => {
    clearTimeout(again.current);
    setPicked(undefined);
    setTool("");
    setChosen(undefined);
    setHover(undefined);
    setCaret(undefined);
    setNoted(undefined);
    setSaved(undefined);
    setUnmade(undefined);
    setSaves(wrote.current);
    setZoom(1);
    setCell(undefined);
    setRuling("");
    setMerging(undefined);
    setRanged(undefined);
    setTold(UNTOLD);
  }, [src]);
  // Presenting: over the app, full screen where the browser allows. Leaving
  // full screen (a presenter's window opening takes it away) keeps presenting;
  // End or Esc stop.
  const [presenting, setPresenting] = useState(false);
  const [notesShown, setNotesShown] = useState(false);
  const present = (on: boolean) => {
    setPresenting(on);
    setNotesShown(on && matchMedia("(orientation: portrait)").matches);
    // Not from a slide that is skipped: from the next one shown.
    const shown = (from: number) =>
      boards.findIndex((board, at) => at >= from && !board.skip);
    if (on && boards[index]?.skip)
      to(shown(index) >= 0 ? shown(index) : shown(0));
    if (on) stage.current?.requestFullscreen?.().catch(() => {});
    else if (document.fullscreenElement) document.exitFullscreen();
  };
  // The presenter's window moves this one, and this one it.
  const broadcast = usePresentChannel(({ at, build }) => {
    if (!at || !boards.some((board) => board.path === at)) return;
    if (at !== path) go(at);
    else if (Number.isInteger(build)) showBuild(build as number);
  });
  // Which of its file's boards shows: a document's page, a sheet's tab.
  const [part, setPart] = useState(0);
  // A sheet's selection, what it offers the formula being typed, the range
  // a comment is being written on, and what the sheet last said of itself.
  const [cell, setCell] = useState<Cell>();
  // What is being typed in the page's cell, for the formula bar to show.
  const [typed, setTyped] = useState<string>();
  const [offer, setOffer] = useState<Offer>();
  // The column whose rules are being set: its dialog is open while the
  // active cell is in it.
  const [ruling, setRuling] = useState("");
  // A merge that would empty cells holding text, asked about first.
  const [merging, setMerging] = useState<{ range: string; count: number }>();
  // The table that cell's edit would make, as its page said it once, and
  // its text; and the edits of the changes the sheet is calculating. All
  // are held, to be written if the sheet is left first.
  const cutNow = useRef<Draft>(undefined);
  const writtenNow = useRef<unknown>(undefined);
  const heldNow = useRef<TableEdit[]>([]);
  const hold = () => {
    const [held, cut, written] = [
      heldNow.current,
      cutNow.current,
      writtenNow.current,
    ];
    typing(
      sheet && (held.length || cut)
        ? () => [...held, ...(cut ? [editOf(cut, written)] : [])]
        : undefined,
      held.length > 0,
    );
  };
  const [ranged, setRanged] = useState<string>();
  const [told, setTold] = useState(UNTOLD);
  useEffect(() => setPart(0), [path]);
  const first = boards.findIndex((board) => board.path === path);
  const index = boards.findLastIndex(
    (board, at) => board.path === path && at <= first + part,
  );
  // A slide's builds while presenting: the step shown, and whether the slide
  // was come to from the one after it, when it shows built.
  const [build, setBuild] = useState(0);
  const backward = useRef(false);
  const builds = presenting && kind === "deck" ? (said?.steps ?? 0) : 0;
  const showBuild = (at: number) => {
    setBuild(at);
    tell(frame.current, { type: "build", at });
  };
  // A document's, a slide's or a whiteboard's changes by hand are undone and
  // made again here; then the file is drawn again, its notes its own.
  const undoes = !!undo && editable;
  const back = (redo: boolean) =>
    void undo?.(redo).then((made) => {
      if (!made) return;
      wrote.current++;
      setSaves(wrote.current);
      setNoted(undefined);
      setLoad((count) => count + 1);
    });
  // Whether the key undoes or redoes: ⌘Z, with Shift, or Ctrl+Y.
  const undoKey = (key: string, shift?: boolean) => {
    const letter = key.toLowerCase();
    if (!undoes || (letter !== "z" && letter !== "y")) return false;
    back(letter === "y" || !!shift);
    return true;
  };
  // Presenting starts at the slide's first step; ended, all of it shows.
  // biome-ignore lint/correctness/useExhaustiveDependencies: on presenting alone
  useEffect(() => {
    if (kind === "deck") showBuild(presenting ? 0 : -1);
  }, [presenting]);
  const stepBuild = (at: number) => {
    showBuild(at);
    broadcast({ at: path, build: at });
  };
  // Another file is another view, and the presenter's window goes with it.
  const to = (next: number, back = false) => {
    const going = boards[next];
    if (!going) return;
    backward.current = back;
    if (going.path !== path) {
      go(going.path);
      broadcast({ at: going.path });
    }
    setPart(going.at ?? 0);
  };
  // The boards stepped through and counted: all, but while presenting only
  // the one shown and those not skipped.
  const stops = boards.flatMap((board, at) =>
    at === index || !(presenting && board.skip) ? [at] : [],
  );
  const place = stops.indexOf(index);
  // Past the last slide comes the first; a document ends at its ends. A
  // presented slide steps through its builds first.
  const step = (by: number) => {
    if (by > 0 && build < builds) return stepBuild(build + 1);
    if (by < 0 && build > 0 && builds) return stepBuild(build - 1);
    return (
      place >= 0 &&
      to(
        stops[
          doc
            ? Math.min(stops.length - 1, Math.max(0, place + by))
            : (place + by + stops.length) % stops.length
        ],
        by < 0,
      )
    );
  };
  // Whether the key was one that steps; Home and End go to the ends.
  const stepKey = (key: string) => {
    const by = STEPS[key];
    if (by) step(by);
    else if (key === "Home") to(stops[0]);
    else if (key === "End") to(stops[stops.length - 1]);
    else return false;
    return true;
  };
  // With ⌘ or Ctrl the paging keys show a sheet's next tab, or the one
  // before.
  const tabKey = (key: string) => {
    const by = { PageDown: 1, PageUp: -1 }[key];
    if (by) to(index + by);
    return !!by;
  };
  const presentKey = (key: string) => {
    stepKey(key);
    if (key === "n" || key === "N") setNotesShown((shown) => !shown);
    if (key === "Escape") present(false);
  };
  // The arrows step from anywhere but a field; the paging keys only while the
  // pane has them (the transcript pages with the same keys). Read as they are
  // at the press: a key right after a step counts from where that step led.
  const keys = useLatest((event: KeyboardEvent) => {
    const target = event.target as Element;
    const modal = target.closest?.("dialog");
    // Not a key another widget took, nor one in a dialog opened over the
    // pane, nor Space on a button, which presses it.
    if (
      event.defaultPrevented ||
      (modal && !modal.contains(stage.current)) ||
      (event.key === " " && target.closest?.("button"))
    )
      return;
    const here = target === document.body || target.closest?.(".files");
    const held = event.metaKey || event.ctrlKey;
    if (event.key === "Escape" && tool) arm("");
    if (presenting) presentKey(event.key);
    else if (sheet) {
      if (here && held && tabKey(event.key)) event.preventDefault();
    } else if (target.closest?.("input, textarea, select, [contenteditable]"))
      return;
    else if (here && held && undoKey(event.key, event.shiftKey))
      event.preventDefault();
    else if (
      boards.length > 1 &&
      !event.altKey &&
      !held &&
      (event.key.startsWith("Arrow") || here) &&
      stepKey(event.key)
    )
      event.preventDefault();
  });
  useEffect(() => {
    const pressed = (event: KeyboardEvent) => keys.current(event);
    document.addEventListener("keydown", pressed);
    return () => document.removeEventListener("keydown", pressed);
  }, [keys]);
  // What is being saved, said once: a block's edits as it is typed in share
  // one save.
  const [saved, setSaved] = useState<{ text: string; failed?: true }>();
  const refused = (text: string) => setSaved({ text, failed: true });
  // Why a sheet did not make a change, until the next one is asked of it: a
  // change asked before it may be saved after.
  const [unmade, setUnmade] = useState<{ text: string }>();
  const failure = unmade ?? (saved?.failed ? saved : undefined);
  // A slide's notes as typed under its stage, until its page says its own.
  const [noted, setNoted] = useState<string>();
  const notes = noted ?? info?.notes ?? "";
  const keeping = useRef<Promise<unknown>>(undefined);
  const keep = async (saving?: Promise<unknown>) => {
    if (saving && saving === keeping.current) return;
    keeping.current = saving;
    setSaved({ text: "Saving…" });
    try {
      const done = (await saving) as Partial<Saved> | undefined;
      if (done?.owed)
        refused(
          `Saved, but a block typed in before is not in the file: ${done.owed}`,
        );
      else setSaved({ text: "Saved" });
      if (editable) wrote.current++;
    } catch (error) {
      refused(error instanceof Error ? error.message : "Not saved");
    }
  };
  // A window opened in the press that asks for it and sent to its address
  // once what was typed is in the file, so it shows the file as it then is.
  const open = (address: string) => {
    const made = window.open("", "_blank");
    if (!made) return;
    made.opener = null;
    void settle().then(() => made.location.replace(address));
  };
  const stem = nameOf(path).replace(/\.[^.]+$/, "");
  // An Excel or CSV file is only read: its page takes no change.
  const locked = sheet && SHEET_FILE.test(path);
  // What the sheet was last told to do: a file comes only as its answer.
  const asked = useRef("");
  const act = (command: string, value?: string) => {
    asked.current = command;
    tell(frame.current, { type: "do", command, value, tab: index });
    if (TAB_AFTER[command]) setPart(TAB_AFTER[command](index, boards.length));
  };
  // Its size as a picture: an artboard's, or a page's full height.
  // The page's shape: what it said once drawn, and until then what its file
  // names, so the stage does not move when it arrives.
  const shape = info?.artboard ?? boards[index]?.named;
  const size = pageSize(
    info ?? (shape && ({ artboard: shape } as PageInfo)),
    8000,
  );
  const { fixed } = size;
  const sized = `&w=${size.w}&h=${size.h}`;
  // Of a file with several boards, the one shown is pictured.
  const shown = boards[index]?.at;
  const shownPart = shown === undefined ? "" : `&part=${shown + 1}`;
  const png =
    drawn &&
    info &&
    ((by: number) => `${drawn("png", path)}${sized}&scale=${by}${shownPart}`);
  // What the host draws is of the file: asked for once what was typed is in
  // it.
  const download = (address: string, name: string) =>
    saveDrawn(settle, address, name);
  // The page in another format: what fits its kind. Its source is Download
  // in the pane's header.
  const exports: (false | undefined | [string, () => void])[] = [
    drawn
      ? ["PDF", () => download(drawn("pdf", path), `${stem}.pdf`)]
      : ["Print or save as PDF", () => open(`${src}#print`)],
    drawn &&
      kind === "deck" && [
        "PDF, a page per slide",
        () =>
          download(
            `${drawn("pdf", `${dirOf(path)}/`)}${sized}`,
            `${nameOf(dirOf(path)) || "deck"}.pdf`,
          ),
      ],
    png && ["PNG image", () => download(png(2), `${stem}.png`)],
    png && ["Copy as image", () => copyDrawn(settle, png(2))],
    live &&
      doc && ["Markdown", () => tell(frame.current, { type: "markdown" })],
    live &&
      sheet &&
      !/\.xlsx$/i.test(path) && ["Excel workbook", () => act("xlsx")],
    live && sheet && ["CSV of this tab", () => act("csv")],
  ];
  // A fixed artboard fits the stage whole; a page that names only its width
  // fits it across and scrolls down inside.
  const wide = !!shape?.w;
  const fit =
    room.w && wide
      ? Math.min(1, room.w / shape!.w, fixed ? room.h / shape!.h : 1)
      : 1;
  // How far the person zoomed from the fit; each page opens at the fit.
  const [zoom, setZoom] = useState(1);
  // On a touch screen the tools for items changed by hand, or a document's
  // inserts, take the foot of the stage (two fingers zoom a whiteboard): Fit
  // is in the tools' menu.
  const touch = (!!hand || (doc && mode === "edit")) && coarsePointer();
  const scale = fit * zoom;
  // The handles of items changed by hand keep a finger's size at any zoom.
  useEffect(() => {
    if (live && hand)
      tell(frame.current, {
        type: "tweak",
        name: "--uagent-scale",
        value: scale,
      });
  }, [live, scale, hand]);
  // Scales by `by` keeping the point (x, y) of the scroller where it is.
  const anchor = useRef<{ x: number; y: number; cx: number; cy: number }>(
    undefined,
  );
  const zoomAt = (by: number, x: number, y: number) => {
    const box = scroller.current;
    if (!box) return;
    anchor.current = {
      x,
      y,
      cx: (box.scrollLeft + x) / scale,
      cy: (box.scrollTop + y) / scale,
    };
    setZoom((was) =>
      Math.min(ZOOM_MAX / fit, Math.max(ZOOM_MIN / fit, was * by)),
    );
  };
  useLayoutEffect(() => {
    const box = scroller.current;
    const at = anchor.current;
    if (!box || !at) return;
    box.scrollLeft = at.cx * scale - at.x;
    box.scrollTop = at.cy * scale - at.y;
    anchor.current = undefined;
  }, [scale]);
  // The middle of the view, for − and +.
  const middle = (by: number) =>
    scroller.current &&
    zoomAt(
      by,
      scroller.current.clientWidth / 2,
      scroller.current.clientHeight / 2,
    );
  // Wheel listeners keep the current fit and scale when the page resizes.
  const wheelZoom = useLatest(zoomAt);
  useEffect(() => {
    const box = scroller.current;
    if (box)
      return bindZoomWheel(box, (by, at) => wheelZoom.current(by, at.x, at.y));
  }, [wheelZoom]);
  // The design system the page links, by its tokens.css.
  const linked = info?.sheets?.find((href) => /\/tokens\.css(\?|$)/.test(href));
  const system = linked
    ? decodeURIComponent(new URL(linked).pathname.slice(base.length))
    : "";
  const theme = useResource(
    () => (system ? read(system).then(readTokens) : undefined),
    [system],
  );
  const declared = Object.entries(
    (info?.tweaks ?? {}) as Record<string, Tweak>,
  );
  // Find: a deck's or a design's occurrences are counted in the boards'
  // source, so a step reaches a board that is not drawn, and the stage marks
  // those of the one shown. Only a document's layout knows which page a word
  // is on, and a sheet's grid what its cells show: their frames count them.
  const counted = doc || sheet;
  const [finding, setFinding] = useState(false);
  const [words, setWords] = useState("");
  const [found, setFound] = useState<number[]>();
  const query = finding ? words.trim() : "";
  // The current occurrence, for the words it was stepped to in: new words
  // start at the first.
  const [stepped, setStepped] = useState({ query, spot: 0 });
  const spot = stepped.query === query ? stepped.spot : 0;
  // Tagged with what was read: another deck's texts, or a slide's from
  // before it was rewritten, are never taken for these.
  const sources = useResource(
    () =>
      finding && !counted
        ? Promise.all(
            boards.map((board) =>
              read(board.path).then((html) => drawnText(html, kind === "deck")),
            ),
          ).then((texts) => ({ stamp, texts }))
        : undefined,
    [finding, counted, stamp],
  );
  const texts =
    sources.value?.stamp === stamp ? sources.value.texts : undefined;
  const counts = useMemo(
    () =>
      counted
        ? found
        : texts?.map((nodes) =>
            sum(nodes.map((text) => occurrences(text, query).length)),
          ),
    [counted, found, texts, query],
  );
  const hits = !query || !counts ? undefined : sum(counts);
  const hit = spotAt(counts ?? [], spot);
  const show = (next: number) => {
    setStepped({ query, spot: next });
    const { at } = spotAt(counts ?? [], next);
    if (hits && at !== index) to(at);
  };
  // Counted: the first occurrence on this board or after it.
  // biome-ignore lint/correctness/useExhaustiveDependencies: lands once per count; a step taken since must not move it
  useEffect(() => {
    const before = sum((counts ?? []).slice(0, index));
    show(hits && before < hits ? before : 0);
  }, [query, counts]);
  // What the stage's page marks: the current occurrence, when it is there.
  const nth = hit.at === index ? hit.nth : -1;
  const marked = () =>
    tell(frame.current, { type: "find", text: query, index: nth });
  useEffect(marked, [query, nth]);
  const mark = (rect: Rect, kind: string) => (
    <span
      class={`page-mark ${kind}`}
      style={{
        left: `${rect.x * scale}px`,
        top: `${rect.y * scale}px`,
        width: `${rect.width * scale}px`,
        height: `${rect.height * scale}px`,
      }}
    />
  );
  // What a comment is on: the element picked on a page, or a sheet's range.
  const about = picked
    ? {
        label: named(picked, picked.tag),
        where: named(picked, `\`${picked.selector}\``),
      }
    : ranged && { label: ranged, where: `\`${ranged}\`` };
  const closeComment = () => {
    setPicked(undefined);
    setRanged(undefined);
  };
  const [legend, setLegend] = useState(true);
  // A deck's slides all at once, in place of the stage.
  const [grid, setGrid] = useState(false);
  const gridded = kind === "deck" && boards.length > 1;
  // A tool that is on or off: named, or `bare` its icon alone.
  const toggle = (
    label: string,
    on: boolean,
    press: () => void,
    icon: ComponentChildren,
    bare?: boolean,
    // Why it cannot be pressed, when it cannot.
    off?: string,
  ) =>
    bare ? (
      <IconButton label={label} aria-pressed={on} onClick={press}>
        {icon}
      </IconButton>
    ) : (
      <Button
        variant="secondary"
        aria-pressed={on}
        disabled={!!off}
        title={off}
        onClick={press}
      >
        {icon}
        {label}
      </Button>
    );
  const flip = (to: "select" | "edit") => enter(mode === to ? "" : to);
  // What is being saved: a block's in the row of Edit's own, anything
  // else's in the tools' row. Last there, it takes the room the tools leave,
  // so no tool moves for it. Why something was not saved or done is said
  // over the tools, whole: a row would cut it short.
  const status = saved && !failure && (
    <small class="page-status muted">
      <Notice tone="info">{saved.text}</Notice>
    </small>
  );
  const whole = doc ? "document" : sheet || kind === "deck" ? kind : "design";
  // A deck rearranged from here, by a drag in the legend or from the slide's
  // menu: the stage follows its slide.
  const names = boards.map((board) => nameOf(board.path));
  const rearrange = (moves: Move[]) =>
    moves.length && arrange?.(moves, shownAfter(moves, names, index));
  const stepper = useRef<HTMLDivElement>(null);
  const counter = (boards.length > 1 || arrange) && !sheet && (
    // raw: floats over the page, not along an edge of a pane.
    <div
      class="float-toolbar page-count"
      role="group"
      aria-label="Step"
      ref={stepper}
    >
      <IconButton
        label={`Previous ${unit.toLowerCase()}`}
        disabled={doc && place === 0}
        onClick={() => step(-1)}
      >
        <ChevronLeft />
      </IconButton>
      <output>
        {place + 1} / {stops.length}
      </output>
      <IconButton
        label={`Next ${unit.toLowerCase()}`}
        disabled={doc && place === stops.length - 1}
        onClick={() => step(1)}
      >
        <ChevronRight />
      </IconButton>
      {arrange && !presenting && (
        <SlideActions
          slides={names}
          index={index}
          title={boards[index]?.title}
          skipped={boards[index]?.skip}
          busy={arranging}
          act={rearrange}
          add={async () => {
            const { name, moves } = added(names, index);
            arrange(moves, name, { name, text: blankSlide(await read(path)) });
          }}
          pushed={info?.push}
          skip={() =>
            rewrite?.(
              path,
              (source) =>
                markSlide(source, "skip", !boards[index]?.skip) ||
                refuse(false),
            )
          }
          push={async () => {
            const push = !info?.push;
            // The page is not drawn again for its own mark: it is as written.
            if (
              await rewrite?.(
                path,
                (source) => markSlide(source, "push", push) || refuse(false),
              )
            )
              setSaid((was) => was && { ...was, push });
          }}
        />
      )}
    </div>
  );
  const exportItems = exports.map(
    (item) =>
      item && (
        <MenuItem key={item[0]} onClick={item[1]}>
          {item[0]}
        </MenuItem>
      ),
  );
  // What the stage's scroller shows, short of its scroll bars.
  const inside = () => {
    const box = scroller.current;
    return box
      ? { width: box.clientWidth, height: box.clientHeight }
      : undefined;
  };
  // A box of the page, in the window.
  const inWindow = (rect?: Rect) => {
    const page = frame.current?.getBoundingClientRect();
    return (
      page &&
      rect && {
        x: page.left + rect.x * scale,
        y: page.top + rect.y * scale,
        width: rect.width * scale,
        height: rect.height * scale,
      }
    );
  };
  const format = (said: Format) =>
    tell(frame.current, { type: "format", ...said });
  const textTools = caret && (
    <TextTools
      caret={caret}
      format={format}
      linking={linking}
      link={() => setLinking(!linking)}
    />
  );
  const draw = () =>
    png &&
    settle().then(() => view({ src: png(1), name: `${stem}.png`, edit: true }));
  return (
    <div class="open-page">
      {failure && (
        <p class="page-notice">
          <Notice tone="error">{failure.text}</Notice>
        </p>
      )}
      {/* raw: rows, not a row: on a phone a sheet's two share one. */}
      <div class="page-head">
        <Bar edge="bottom" class="page-tools">
          {boards.length > 1 &&
            !sheet &&
            toggle(
              "Legend",
              legend,
              () => setLegend(!legend),
              <PanelLeft />,
              true,
            )}
          {!sheet &&
            toggle(
              "Select",
              mode === "select",
              () => flip("select"),
              <MousePointerClick />,
            )}
          {save &&
            editable &&
            toggle(
              "Edit",
              mode === "edit",
              () => flip("edit"),
              <Pencil />,
              false,
              uneditable,
            )}
          {undoes && (
            <>
              <IconButton
                label="Undo"
                disabled={!undoable?.undo}
                onClick={() => back(false)}
              >
                <Undo2 />
              </IconButton>
              <IconButton
                label="Redo"
                disabled={!undoable?.redo}
                onClick={() => back(true)}
              >
                <Redo2 />
              </IconButton>
            </>
          )}
          {(declared.length > 0 || !!theme.value?.colors.length) &&
            toggle(
              "Tweaks",
              tweaking,
              () => setTweaking(!tweaking),
              <SlidersHorizontal />,
            )}
          {/* raw: a narrow pane folds the tools after Edit into one menu. */}
          <span class="page-tools-wide">
            {gridded &&
              toggle("Grid", grid, () => setGrid(!grid), <LayoutGrid />, true)}
            {toggle(
              "Find",
              finding,
              () => setFinding(!finding),
              <Search />,
              true,
            )}
            {!sheet && (
              <IconButton label="Present" onClick={() => present(true)}>
                <Presentation />
              </IconButton>
            )}
            {drawn && (
              <IconButton label="Draw on it" disabled={!png} onClick={draw}>
                <PenLine />
              </IconButton>
            )}
            <Menu
              label="Export"
              trigger={
                <>
                  <FileOutput />
                  Export
                  <ChevronDown />
                </>
              }
            >
              {exportItems}
            </Menu>
            <IconButton label="Open alone" onClick={() => open(src)}>
              <ExternalLink />
            </IconButton>
          </span>
          <span class="page-tools-folded">
            <Menu label="More tools">
              {gridded && (
                <MenuItem
                  role="menuitemcheckbox"
                  aria-checked={grid}
                  onClick={() => setGrid(!grid)}
                >
                  Grid
                  {grid && <Check />}
                </MenuItem>
              )}
              <MenuItem onClick={() => setFinding(!finding)}>Find</MenuItem>
              {!sheet && (
                <MenuItem onClick={() => present(true)}>Present</MenuItem>
              )}
              {drawn && (
                <MenuItem disabled={!png} onClick={draw}>
                  Draw on it
                </MenuItem>
              )}
              <MenuSub label="Export">{exportItems}</MenuSub>
              <MenuItem onClick={() => open(src)}>Open alone</MenuItem>
              {touch && (
                <MenuItem disabled={zoom === 1} onClick={() => setZoom(1)}>
                  Fit
                </MenuItem>
              )}
            </Menu>
          </span>
          {writing && <span class="chip">Agent writing</span>}
          {!board && status}
        </Bar>
        {sheet && (
          <fieldset class="page-asks" disabled={!live}>
            <FormulaBar
              cell={cell}
              typing={typed}
              locked={locked}
              type={(value) => tell(frame.current, { type: "input", value })}
              act={act}
              offer={offer}
              ask={(text) => tell(frame.current, { type: "suggest", text })}
            />
          </fieldset>
        )}
      </div>
      {linking && caret && (
        <LinkField
          key={caret.href}
          href={caret.href}
          apply={(href) => {
            tell(frame.current, { type: "format", href });
            setLinking(false);
            frame.current?.focus();
          }}
          close={() => setLinking(false)}
        />
      )}
      {finding && (
        <FindField
          label={`Find in this ${whole}`}
          words={words}
          setWords={setWords}
          count={hits}
          current={spot}
          step={(by) => hits && show((spot + by + hits) % hits)}
          replace={
            sheet && !locked
              ? (by, all) =>
                  tell(frame.current, {
                    type: "replace",
                    text: query,
                    with: by,
                    index: all ? -1 : nth,
                  })
              : undefined
          }
          close={() => setFinding(false)}
        />
      )}
      <div class="page-body" data-grid={grid || undefined}>
        {(legend || grid) && boards.length > 1 && !sheet && (
          <Legend
            unit={unit}
            boards={boards}
            srcs={srcs}
            stamp={stamp}
            index={index}
            heard={heard}
            grid={grid}
            to={(at) => {
              to(at);
              setGrid(false);
            }}
            move={
              arrange && !arranging
                ? (from, place) => rearrange(moved(names, from, place))
                : undefined
            }
            // A right click or the menu key on a slide: its menu.
            menu={
              arrange &&
              ((at) => {
                to(at);
                stepper.current
                  ?.querySelector<HTMLElement>(".action-menu button")
                  ?.click();
              })
            }
          />
        )}
        <div class="page-main">
          <div
            class="page-stage"
            ref={stage}
            data-fixed={fixed || undefined}
            data-wide={(wide && !fixed) || undefined}
            data-presenting={presenting || undefined}
          >
            {!info && <Spinner label="Opening the page…" />}
            <div class="page-scroll" ref={scroller}>
              <div
                class="page-holder"
                // The first page fades in once it has drawn; one after it
                // takes its place in the frame.
                data-arriving={!info || undefined}
                style={
                  wide
                    ? {
                        width: `${shape!.w * scale}px`,
                        height: `${fixed ? shape!.h * scale : room.h}px`,
                      }
                    : undefined
                }
              >
                <Frame
                  src={
                    boards[index]
                      ? address(boards[index], load ? `&load=${load}` : "") +
                        (presenting && kind === "deck"
                          ? `#build=${backward.current ? 99 : 0}`
                          : "")
                      : src
                  }
                  title={info?.title || path}
                  transition={
                    presenting && kind === "deck"
                      ? said?.push
                        ? "push"
                        : "fade"
                      : undefined
                  }
                  frameRef={frame}
                  width={wide ? shape!.w : undefined}
                  height={fixed ? shape!.h : wide ? room.h / scale : undefined}
                  scale={scale}
                  onMessage={(message) => {
                    // The boards hear what fails here, and all of a document
                    // or a sheet, whose legend may be shut. Any other page is
                    // measured by its picture: here it is as tall as the stage.
                    if (
                      doc ||
                      sheet ||
                      message.type === "error" ||
                      message.type === "issue"
                    )
                      heard(path)(message);
                    if (message.type === "ready") {
                      setSaid(message);
                      // A page drawn again (or a change taken in place, after
                      // which the board says what is still picked) holds no
                      // tool.
                      arm("");
                      setChosen(undefined);
                      setBuild(backward.current ? (message.steps ?? 0) : 0);
                      // Drawn from the file: what it does not hold of what
                      // was typed is on no page any more.
                      forget();
                      if (mode)
                        tell(frame.current, {
                          type: mode,
                          on: true,
                          ...caretAt.current,
                        });
                      if (caretAt.current) frame.current?.focus();
                      caretAt.current = undefined;
                      // Drawn again, what was found stays marked.
                      if (query) marked();
                    } else if (message.type === "update") {
                      if (message.result === "held") {
                        again.current = setTimeout(
                          () => renew.current(),
                          RENEW_MS,
                        );
                        return;
                      }
                      if (message.result === "load")
                        setLoad((count) => count + 1);
                      // The legend draws the file again.
                      wrote.current++;
                      setSaves(wrote.current);
                    } else if (message.type === "drawn") {
                      setSaid((was) => was && { ...was, drawn: true });
                      setArrived(wanted);
                      // A sheet's menus offer a comment where one can be made.
                      if (sheet && addToChat)
                        tell(frame.current, {
                          type: "do",
                          command: "comments",
                        });
                    } else if (message.type === "zoom") {
                      // A point of the page, where it sits in the scroller.
                      const page = frame.current!.getBoundingClientRect();
                      const box = scroller.current!.getBoundingClientRect();
                      zoomAt(
                        message.by,
                        page.left + message.x * scale - box.left,
                        page.top + message.y * scale - box.top,
                      );
                    } else if (message.type === "size")
                      // A page that grows (its fonts, its images) is
                      // pictured whole.
                      setSaid(
                        (was) => was && { ...was, height: message.height },
                      );
                    else if (message.type === "find") {
                      // A slide's are counted in its source, not here. Said
                      // again the same, a document's counts are no news.
                      const counts = Array.isArray(message.counts)
                        ? message.counts.slice(0, boards.length).map(natural)
                        : undefined;
                      if (counted)
                        setFound((was) =>
                          String(was) === String(counts) ? was : counts,
                        );
                    } else if (message.type === "page") {
                      // A link in the document, or its caret, led there.
                      if (doc) to(natural(message.at) - 1);
                    } else if (message.type === "edit") {
                      // The page is untrusted: an edit is taken only where
                      // it is on offer. A slide takes no new block.
                      if ("table" in message) {
                        if (!sheet) return;
                        const [undo, redo] = [!!message.undo, !!message.redo];
                        setTold((was) => ({ ...was, undo, redo }));
                        void keep(save?.(message));
                      } else if (
                        (mode === "edit" && editable) ||
                        (doc && typeof message.done === "boolean")
                      ) {
                        const edit = {
                          ...message,
                          insert: doc ? message.insert : undefined,
                        };
                        const saving = save?.(edit);
                        void keep(saving);
                        // Another kind of block, or a new one, is the
                        // file's to make: written at once, the page is
                        // drawn again from it.
                        if (!edit.as && !edit.insert) return;
                        void settle();
                        void saving?.then(
                          (saved) => {
                            caretAt.current = {
                              at: natural((saved as Saved).at),
                              over: !!edit.insert,
                            };
                            setLoad((count) => count + 1);
                            setSaves(wrote.current);
                          },
                          () => {},
                        );
                      }
                    } else if (message.type === "source") {
                      const { rect, label, path: from, line } = message;
                      clearTimeout(unsourced.current);
                      if (rect && typeof from === "string")
                        setSource({
                          rect,
                          label: String(label ?? "").slice(0, 200),
                          path: from,
                          line: natural(line),
                        });
                      // A moment, for the pointer to reach the chip.
                      else
                        unsourced.current = setTimeout(
                          () => !holding.current && setSource(undefined),
                          300,
                        );
                    } else if (message.type === "change") {
                      if (!hand) return;
                      // An item put on the page ends its tool.
                      if ("add" in message || "arrow" in message) setTool("");
                      // One the file does not take is said, and the page is
                      // drawn again from the file; one it takes, the legend
                      // draws.
                      let taken = true;
                      void rewrite?.(path, (source) => {
                        const made = hand.edit(source, message as never);
                        taken = typeof made === "string";
                        return made || refuse(made as false | null);
                      }).then(() => {
                        if (!taken) return setLoad((count) => count + 1);
                        wrote.current++;
                        setSaves(wrote.current);
                      });
                    } else if (message.type === "items") {
                      setChosen(hand ? chosenOf(message) : undefined);
                    } else if (message.type === "caret") {
                      setCaret(caretOf(message));
                    } else if (message.type === "left") {
                      // Once its last edit is written.
                      void keeping.current?.then(
                        () => setSaves(wrote.current),
                        () => {},
                      );
                    } else if (message.type === "file") {
                      // The page is untrusted: only what was asked of a
                      // sheet is taken. Its text is asked of a read file alone.
                      const made =
                        sheet && message.as === asked.current
                          ? fileOf(message)
                          : undefined;
                      asked.current = "";
                      if (typeof made === "string") refused(made);
                      else if (made?.as === "sheet")
                        void keep(saveAs?.(made.text));
                      else if (made)
                        downloadBlob(made.blob, `${stem}.${made.as}`);
                    } else if (message.type === "markdown")
                      downloadBlob(
                        new Blob([message.text], { type: "text/markdown" }),
                        `${stem}.md`,
                      );
                    else if (message.type === "hover") setHover(message.rect);
                    else if (message.type === "pick") setPicked(message);
                    else if (message.type === "picked")
                      setPicked((was) => was && { ...was, rect: message.rect });
                    else if (message.type === "sheet") {
                      // Drawn: a find asked for before it listened is
                      // asked again.
                      if (query) marked();
                      const hidden = Number(message.hidden) || 0;
                      const readers = Number(message.readers) || 0;
                      setTold((was) => ({ ...was, hidden, readers }));
                    } else if (message.type === "cell") {
                      const said = cellOf(message);
                      setTyped(said.typing);
                      setCell(said);
                      setRuling((was) => (was === columnOf(said) ? was : ""));
                    } else if (message.type === "typing") {
                      const { text, written, first } = message;
                      setTyped(
                        text === undefined
                          ? undefined
                          : String(text).slice(0, 8192),
                      );
                      // A draft's table is its own, said with its first
                      // text, or none: never one said before.
                      const cut =
                        text === undefined
                          ? undefined
                          : first
                            ? draftOf(message)
                            : cutNow.current;
                      cutNow.current = cut;
                      writtenNow.current = written;
                      hold();
                    } else if (message.type === "suggest")
                      setOffer(offerOf(message));
                    else if (message.type === "rules")
                      setRuling(sheet && !locked && cell ? columnOf(cell) : "");
                    else if (message.type === "merge")
                      setMerging(
                        sheet && !locked
                          ? {
                              range: String(message.range).slice(0, 80),
                              count: Number(message.count) || 0,
                            }
                          : undefined,
                      );
                    else if (message.type === "refused")
                      setUnmade(
                        message.message
                          ? { text: String(message.message).slice(0, 300) }
                          : undefined,
                      );
                    else if (message.type === "calculating")
                      setSaved((was) =>
                        message.on
                          ? { text: CALCULATING }
                          : was?.text === CALCULATING
                            ? undefined
                            : was,
                      );
                    else if (message.type === "held") {
                      heldNow.current = heldOf(message) ?? [];
                      hold();
                    } else if (message.type === "link") {
                      // The page is untrusted: only these open.
                      const href = String(message.href);
                      if (/^(https?|mailto):/i.test(href))
                        window.open(href, "_blank", "noopener");
                    } else if (message.type === "comment") {
                      if (sheet && addToChat) setRanged(cell?.range);
                    } else if (message.type === "key") {
                      if (message.key === "Escape") setTool("");
                      // A sheet's other keys are its own.
                      if (sheet) {
                        if (message.held) tabKey(message.key);
                      } else if (presenting) presentKey(message.key);
                      else if (message.held)
                        undoKey(message.key, message.shift);
                      else if (mode !== "select") stepKey(message.key);
                      else if (message.key === "Escape") enter("");
                    }
                  }}
                />
                {mode === "select" && hover && mark(hover, "hover")}
                {source && !mode && (
                  // raw: laid under a value of the page, at the pane's size.
                  <span
                    class="float-toolbar page-source small"
                    style={{
                      left: `${source.rect.x * scale}px`,
                      top: `${(source.rect.y + source.rect.height) * scale}px`,
                    }}
                    onPointerEnter={() => {
                      holding.current = true;
                    }}
                    onPointerLeave={() => {
                      holding.current = false;
                      setSource(undefined);
                    }}
                  >
                    <span>{source.label}</span>
                    {cite && (
                      <Button
                        size="compact"
                        onClick={() => cite(source.path, source.line)}
                      >
                        Open
                      </Button>
                    )}
                  </span>
                )}
                {picked && mark(picked.rect, "picked")}
              </div>
            </div>
            {!presenting && counter}
            {/* Shown, the notes keep their room on a slide that has none, so
                the slide does not move from one to the next. */}
            {presenting && notesShown && <p class="page-notes">{notes}</p>}
            {presenting && (
              // raw: laid over a slide while presenting.
              <div class="page-present-bar">
                {counter}
                <Button
                  variant="secondary"
                  disabled={!notes && !notesShown}
                  aria-pressed={notesShown}
                  onClick={() => setNotesShown(!notesShown)}
                >
                  Notes
                </Button>
                {presenter && kind === "deck" && (
                  <Button
                    variant="secondary"
                    onClick={() =>
                      presenter(
                        stops.map((at) => boards[at].path),
                        path,
                      )
                    }
                  >
                    Presenter view
                  </Button>
                )}
                <Button variant="secondary" onClick={() => present(false)}>
                  End
                </Button>
              </div>
            )}
            {hand && chosen && live && !presenting && (
              <ItemTools
                items={hand}
                chosen={chosen}
                tool={tool}
                arm={arm}
                act={(act, value) =>
                  tell(frame.current, { type: "act", act, value })
                }
                ask={
                  addToChat &&
                  (() => {
                    const item = chosen.items[0];
                    if (item && chosen.rect)
                      setPicked({
                        rect: chosen.rect,
                        selector: item.selector,
                        tag: item.shape ?? (item.note ? "note" : item.tag),
                        text: item.text,
                      });
                  })
                }
                at={() => inWindow(chosen.rect)}
                room={inside}
                // A slide's text takes Edit's tools; a whiteboard's does not.
                text={hand === SLIDE && textTools}
              />
            )}
            {mode === "edit" && doc && live && !presenting && (
              <>
                {caret?.selected && (
                  <PickBar
                    label="Text"
                    at={() => inWindow(caret.rect)}
                    room={inside}
                    // A touch screen's own menu stands over a selection.
                    below={coarsePointer()}
                  >
                    {textTools}
                    {addToChat && (
                      <IconButton
                        label="Ask about it"
                        onClick={() =>
                          setPicked({
                            rect: caret.rect,
                            selector: caret.selector,
                            tag: caret.tag,
                            text: caret.text,
                          })
                        }
                      >
                        <MessageSquare />
                      </IconButton>
                    )}
                  </PickBar>
                )}
                <fieldset
                  class="page-asks"
                  disabled={!caret}
                  onMouseDown={(event) => event.preventDefault()}
                >
                  <InsertBar
                    tools={DOC_INSERTS}
                    armed=""
                    arm={(insert) => format({ insert })}
                  />
                </fieldset>
              </>
            )}
            {sheet && !locked && (
              <fieldset class="page-asks" disabled={!live}>
                <FormatBar
                  cell={cell}
                  act={act}
                  comment={addToChat && (() => setRanged(cell?.range))}
                />
              </fieldset>
            )}
            {wide && !presenting && !touch && (
              <ZoomControls
                percent={Math.round(scale * 100)}
                out={() => middle(1 / ZOOM_STEP)}
                in={() => middle(ZOOM_STEP)}
                fit={zoom !== 1 ? () => setZoom(1) : undefined}
              />
            )}
          </div>
          {presenting ? null : kind === "deck" && mode === "edit" ? (
            <label class="page-notes">
              <span class="muted">Notes</span>
              <Textarea
                aria-label="Speaker notes"
                placeholder="What to say with this slide"
                rows={2}
                disabled={!live}
                value={notes}
                onInput={({ currentTarget: field }) => {
                  setNoted(field.value);
                  void keep(save?.(notesEdit(info?.notes ?? "", field.value)));
                }}
              />
            </label>
          ) : (
            // A deck's notes have their room under every slide, as the field
            // has when editing: a slide sits where the one before it sat.
            (kind === "deck" || notes) && (
              <p class="page-notes">
                <span class="muted">Notes</span>
                {notes || <span class="muted">None for this slide</span>}
              </p>
            )
          )}
          {sheet && (
            <fieldset class="page-asks" disabled={!live}>
              <SheetBar
                boards={boards}
                index={index}
                to={to}
                cell={cell}
                {...told}
                locked={locked}
                act={act}
              />
            </fieldset>
          )}
          {ruling && cell && (
            <RulesDialog
              cell={cell}
              refusal={failure}
              act={act}
              close={() => setRuling("")}
            />
          )}
          {merging && (
            <ConfirmModal
              title="Merge cells"
              action="Merge"
              close={() => setMerging(undefined)}
              confirm={() => {
                setMerging(undefined);
                act("merge", JSON.stringify(merging));
              }}
            >
              {`Only the top-left cell's content stays: ${plural(merging.count, "other cell")} of ${merging.range} that ${merging.count === 1 ? "holds content is" : "hold content are"} emptied. Undo brings ${merging.count === 1 ? "it" : "them"} back.`}
            </ConfirmModal>
          )}
        </div>
        <Tweaks
          key={src}
          open={tweaking}
          path={path}
          declared={declared}
          theme={theme.value}
          system={system}
          tell={(name, value) =>
            tell(frame.current, { type: "tweak", name, value })
          }
          reload={() => setLoad(load + 1)}
          addToChat={addToChat}
        />
      </div>
      {about && addToChat && (
        <form
          class="page-comment"
          onSubmit={(event) => {
            event.preventDefault();
            if (!comment.trim()) return;
            addToChat(commentLine(path, about.where, comment.trim()), true);
            setComment("");
            closeComment();
          }}
        >
          <span class="small muted">{`Comment on ${about.label}`}</span>
          <Textarea
            aria-label="Comment"
            rows={2}
            value={comment}
            onInput={(event) => setComment(event.currentTarget.value)}
          />
          <Actions>
            <Button variant="secondary" type="button" onClick={closeComment}>
              Cancel
            </Button>
            <Button type="submit" disabled={!comment.trim()}>
              Add comment
            </Button>
          </Actions>
        </form>
      )}
    </div>
  );
}
