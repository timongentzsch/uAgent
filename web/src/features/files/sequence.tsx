import type { ComponentChildren } from "preact";
import {
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import {
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  ExternalLink,
  FileOutput,
  PanelLeft,
  PenLine,
  MousePointerClick,
  Pencil,
  Presentation,
  Search,
  SlidersHorizontal,
} from "lucide-preact";
import {
  Actions,
  Button,
  IconButton,
  Notice,
  Spinner,
} from "../../shared/ui.tsx";
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
import { occurrences } from "../../shared/find.ts";
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
import { FormulaBar, SheetBar } from "./sheet.tsx";
import {
  type Cell,
  cellOf,
  fileOf,
  type Offer,
  offerOf,
  type TableEdit,
} from "./sheet-edit.ts";
import { columnOf, RulesDialog } from "./sheet-rules.tsx";
import SlideActions from "./slide-actions.tsx";
import { type Caret, caretOf, FormatTools, LinkField } from "./format.tsx";
import Tweaks, { type Tweak } from "./tweaks.tsx";
import { dirOf } from "./flow.ts";
import { readTokens } from "./tokens.ts";
import { bindZoomWheel } from "../../shared/zoom.ts";
import { useLatest } from "../../shared/use-latest.ts";
import { useRoom } from "../../shared/layout.ts";
import { ZoomControls } from "../../shared/zoom-surface.tsx";
import type { ViewedImage } from "../../shared/attachments.tsx";
import { presentChannel, STEPS } from "./present.ts";
import {
  type BlockEdit,
  blankSlide,
  notesEdit,
  type Saved,
  skipSlide,
} from "./doc-edit.ts";
import { downloadBlob, saveLink } from "../../shared/share.ts";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { nameOf, SHEET_FILE } from "../../shared/file-types.ts";
import { useResource } from "../../shared/use-resource.ts";

// What a sheet says of itself, before it has.
const UNTOLD = { hidden: 0, undo: false, redo: false, readers: 0 };
// Where the pane goes with a tab put in, copied or moved: the page follows.
const TAB_AFTER: Record<string, (index: number, tabs: number) => number> = {
  "tab-add": (_, tabs) => tabs,
  "tab-copy": (index) => index + 1,
  "tab-right": (index) => index + 1,
  "tab-left": (index) => index - 1,
};

// The zoom of an opened page, as a scale of its real size, and one step.
const ZOOM_MIN = 0.1;
const ZOOM_MAX = 4;
const ZOOM_STEP = 1.25;

// A page opened across the pane with what it is part of beside it: a deck's
// slides, a document's pages, a design's boards. A sheet is its grid, which
// keeps the keys and scrolls in its frame (sheet.tsx).

// The lines it adds to the draft, which the artifact skill reads.
// A number a page said, which is untrusted: whole, and none below zero.
const natural = (said: unknown) =>
  Math.min(Math.max(0, Math.floor(Number(said)) || 0), 1e6);
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
  typing,
  saveAs,
  arrange,
  rewrite,
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
  drawn?: (as: "png" | "pdf", path: string) => string;
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
  // A sheet's cell being typed in, as the edit it would make: written if
  // the sheet is left before the cell is. None once it is typed no more.
  typing: (edit?: TableEdit) => void;
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
  // A slide's file written as `change` makes it of what it holds.
  rewrite?: (path: string, change: (source: string) => string) => void;
  arranging?: boolean;
  // Opens a picture in the image viewer, which can draw on it.
  view: (image: ViewedImage) => void;
  go: (path: string) => void;
  addToChat?: (text: string, keep?: boolean) => void;
}) {
  const doc = kind === "doc";
  const sheet = kind === "sheet";
  // Whether its text is typed in place.
  const editable = doc || kind === "deck";
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
  const [picked, setPicked] = useState<Pick>();
  const [comment, setComment] = useState("");
  const [tweaking, setTweaking] = useState(false);
  // Reset reloads the page: what it declares comes back.
  const [load, setLoad] = useState(0);
  // The address whose page has drawn. What is asked of a page before it has
  // is lost, so the controls that ask are disabled until the one wanted has.
  const [arrived, setArrived] = useState("");
  const wanted = load ? `${src}&load=${load}` : src;
  const live = arrived === wanted;
  // Another page: nothing picked, typed or zoomed carries over.
  useEffect(() => {
    setPicked(undefined);
    setHover(undefined);
    setCaret(undefined);
    setNoted(undefined);
    setSaved(undefined);
    setSaves(wrote.current);
    setZoom(1);
    setCell(undefined);
    setRuling("");
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
  const channel = useRef<BroadcastChannel>(undefined);
  const follow = useLatest(
    (at: unknown) =>
      typeof at === "string" &&
      at !== path &&
      boards.some((board) => board.path === at) &&
      go(at),
  );
  // One channel for the page's life: a new one per render would miss what
  // arrives while it is swapped.
  useEffect(() => {
    const opened = presentChannel();
    opened.onmessage = (event) => follow.current(event.data?.at);
    channel.current = opened;
    return () => opened.close();
  }, []);
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
  const [ranged, setRanged] = useState<string>();
  const [told, setTold] = useState(UNTOLD);
  const formula = useRef<HTMLInputElement>(null);
  useEffect(() => setPart(0), [path]);
  const first = boards.findIndex((board) => board.path === path);
  const index = boards.findLastIndex(
    (board, at) => board.path === path && at <= first + part,
  );
  // Another file is another view, and the presenter's window goes with it.
  const to = (next: number) => {
    const board = boards[next];
    if (!board) return;
    if (board.path !== path) {
      go(board.path);
      channel.current?.postMessage({ at: board.path });
    }
    setPart(board.at ?? 0);
  };
  // The boards stepped through and counted: all, but while presenting only
  // the one shown and those not skipped.
  const stops = boards.flatMap((board, at) =>
    at === index || !(presenting && board.skip) ? [at] : [],
  );
  const place = stops.indexOf(index);
  // Past the last slide comes the first; a document ends at its ends.
  const step = (by: number) =>
    place >= 0 &&
    to(
      stops[
        doc
          ? Math.min(stops.length - 1, Math.max(0, place + by))
          : (place + by + stops.length) % stops.length
      ],
    );
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
    if (presenting) presentKey(event.key);
    else if (sheet) {
      if (here && held && tabKey(event.key)) event.preventDefault();
    } else if (target.closest?.("input, textarea, select, [contenteditable]"))
      return;
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
    tell(frame.current, { type: "do", command, value });
    if (TAB_AFTER[command]) setPart(TAB_AFTER[command](index, boards.length));
  };
  // Its size as a picture: an artboard's, or a page's full height.
  const size = pageSize(info, 8000);
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
    settle().then(() => saveLink(`${address}&download=1`, name));
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
    png && [
      "Copy as image",
      () =>
        navigator.clipboard.write([
          new ClipboardItem({
            "image/png": settle()
              .then(() => fetch(png(2)))
              .then((response) => response.blob()),
          }),
        ]),
    ],
    live &&
      doc && ["Markdown", () => tell(frame.current, { type: "markdown" })],
    live &&
      sheet &&
      !/\.xlsx$/i.test(path) && ["Excel workbook", () => act("xlsx")],
    live && sheet && ["CSV of this tab", () => act("csv")],
  ];
  // A fixed artboard fits the stage whole; a page that names only its width
  // fits it across and scrolls down inside.
  const wide = !!info?.artboard.w;
  const fit =
    room.w && wide
      ? Math.min(
          1,
          room.w / info!.artboard.w,
          fixed ? room.h / info!.artboard.h : 1,
        )
      : 1;
  // How far the person zoomed from the fit; each page opens at the fit.
  const [zoom, setZoom] = useState(1);
  const scale = fit * zoom;
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
  // is on: its frame counts them.
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
      finding && !doc
        ? Promise.all(
            boards.map((board) =>
              read(board.path).then((html) => drawnText(html, kind === "deck")),
            ),
          ).then((texts) => ({ stamp, texts }))
        : undefined,
    [finding, doc, stamp],
  );
  const texts =
    sources.value?.stamp === stamp ? sources.value.texts : undefined;
  const counts = useMemo(
    () =>
      doc
        ? found
        : texts?.map((nodes) =>
            sum(nodes.map((text) => occurrences(text, query).length)),
          ),
    [doc, found, texts, query],
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
  // so no tool moves for it; cut short, its title says it whole.
  const status = saved && (
    <small class="page-status muted" title={saved.text}>
      <Notice tone={saved.failed ? "error" : "info"}>{saved.text}</Notice>
    </small>
  );
  const whole = doc ? "document" : kind === "deck" ? "deck" : "design";
  // A deck rearranged from here, by a drag in the legend or from the slide's
  // menu: the stage follows its slide.
  const names = boards.map((board) => nameOf(board.path));
  const rearrange = (moves: Move[]) =>
    moves.length && arrange?.(moves, shownAfter(moves, names, index));
  const stepper = useRef<HTMLDivElement>(null);
  const counter = (boards.length > 1 || arrange) && !sheet && (
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
          skip={() =>
            rewrite?.(path, (source) => skipSlide(source, !boards[index]?.skip))
          }
        />
      )}
    </div>
  );
  return (
    <div class="open-page">
      <div class="page-tools">
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
        {(declared.length > 0 || !!theme.value?.colors.length) &&
          toggle(
            "Tweaks",
            tweaking,
            () => setTweaking(!tweaking),
            <SlidersHorizontal />,
          )}
        {!sheet &&
          toggle("Find", finding, () => setFinding(!finding), <Search />, true)}
        {!sheet && (
          <IconButton label="Present" onClick={() => present(true)}>
            <Presentation />
          </IconButton>
        )}
        {drawn && (
          <IconButton
            label="Draw on it"
            disabled={!png}
            onClick={() =>
              png &&
              settle().then(() =>
                view({ src: png(1), name: `${stem}.png`, edit: true }),
              )
            }
          >
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
          {exports.map(
            (item) =>
              item && (
                <MenuItem key={item[0]} onClick={item[1]}>
                  {item[0]}
                </MenuItem>
              ),
          )}
        </Menu>
        <IconButton label="Open alone" onClick={() => open(src)}>
          <ExternalLink />
        </IconButton>
        {!editable && status}
      </div>
      {mode === "edit" && (
        <div class="page-tools">
          <FormatTools
            caret={caret}
            format={(format) =>
              tell(frame.current, { type: "format", ...format })
            }
            inserts={doc}
            linking={linking && !!caret}
            link={() => setLinking(!linking)}
          />
          {status}
        </div>
      )}
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
          close={() => setFinding(false)}
        />
      )}
      <div class="page-body">
        {legend && boards.length > 1 && !sheet && (
          <Legend
            unit={unit}
            boards={boards}
            srcs={srcs}
            stamp={stamp}
            index={index}
            heard={heard}
            to={to}
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
          {sheet && (
            <fieldset class="page-asks" disabled={!live}>
              <FormulaBar
                cell={cell}
                typing={typed}
                locked={locked}
                field={formula}
                write={(value) => tell(frame.current, { type: "input", value })}
                act={act}
                offer={offer}
                ask={(text) => tell(frame.current, { type: "suggest", text })}
              />
            </fieldset>
          )}
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
                        width: `${info!.artboard.w * scale}px`,
                        height: `${fixed ? info!.artboard.h * scale : room.h}px`,
                      }
                    : undefined
                }
              >
                <Frame
                  src={
                    boards[index]
                      ? address(boards[index], load ? `&load=${load}` : "")
                      : src
                  }
                  title={info?.title || path}
                  frameRef={frame}
                  width={wide ? info!.artboard.w : undefined}
                  height={
                    fixed ? info!.artboard.h : wide ? room.h / scale : undefined
                  }
                  scale={scale}
                  onMessage={(message) => {
                    // The boards hear what fails here, and all of a document
                    // or a sheet, whose legend may be shut. Any other page is
                    // measured by its picture: here it is as tall as the stage.
                    if (doc || sheet || message.type === "error")
                      heard(path)(message);
                    if (message.type === "ready") {
                      setSaid(message);
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
                    } else if (message.type === "drawn") {
                      setSaid((was) => was && { ...was, drawn: true });
                      setArrived(wanted);
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
                      if (doc)
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
                      } else if (mode === "edit" && editable) {
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
                      const hidden = Number(message.hidden) || 0;
                      const readers = Number(message.readers) || 0;
                      setTold((was) => ({ ...was, hidden, readers }));
                    } else if (message.type === "cell") {
                      const said = cellOf(message);
                      setTyped(undefined);
                      setCell(said);
                      setRuling((was) => (was === columnOf(said) ? was : ""));
                    } else if (message.type === "typing") {
                      const { text, tab, tabs, table } = message;
                      setTyped(
                        text === undefined
                          ? undefined
                          : String(text).slice(0, 8192),
                      );
                      typing(
                        sheet && typeof table === "string"
                          ? { tab: Number(tab), tabs: Number(tabs), table }
                          : undefined,
                      );
                    } else if (message.type === "suggest")
                      setOffer(offerOf(message));
                    else if (message.type === "rules")
                      setRuling(sheet && !locked && cell ? columnOf(cell) : "");
                    else if (message.type === "refused")
                      refused(String(message.message).slice(0, 300));
                    else if (message.type === "key") {
                      // A sheet's other keys are its own.
                      if (sheet) {
                        if (message.held) tabKey(message.key);
                      } else if (presenting) presentKey(message.key);
                      else if (mode !== "select") stepKey(message.key);
                      else if (message.key === "Escape") enter("");
                    }
                  }}
                />
                {mode === "select" && hover && mark(hover, "hover")}
                {picked && mark(picked.rect, "picked")}
              </div>
            </div>
            {!presenting && counter}
            {presenting && notesShown && notes && (
              <p class="page-notes">{notes}</p>
            )}
            {presenting && (
              <div class="page-present-bar">
                {counter}
                <Button
                  variant="secondary"
                  disabled={!notes}
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
            {wide && !presenting && (
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
            notes && (
              <p class="page-notes">
                <span class="muted">Notes</span>
                {notes}
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
                edit={() => formula.current?.focus()}
                comment={addToChat && (() => setRanged(cell?.range))}
              />
            </fieldset>
          )}
          {ruling && cell && (
            <RulesDialog
              cell={cell}
              refusal={saved?.failed ? saved : undefined}
              act={act}
              close={() => setRuling("")}
            />
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
              Add to chat
            </Button>
          </Actions>
        </form>
      )}
    </div>
  );
}
