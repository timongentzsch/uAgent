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
import { Actions, Button, IconButton } from "../../shared/ui.tsx";
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
  type Board,
  type Kind,
  type Move,
  moved,
  pageAt,
  shownAfter,
  spotAt,
  sum,
} from "./boards.ts";
import Legend from "./legend.tsx";
import { FormulaBar, SheetBar } from "./sheet.tsx";
import { type Cell, cellOf, fileOf, type TableEdit } from "./sheet-edit.ts";
import SlideActions from "./slide-actions.tsx";
import Tweaks, { type Tweak } from "./tweaks.tsx";
import { dirOf } from "./flow.ts";
import { readTokens } from "./tokens.ts";
import { bindZoomWheel } from "../../shared/zoom.ts";
import { useLatest } from "../../shared/use-latest.ts";
import { useRoom } from "../../shared/layout.ts";
import { ZoomControls } from "../../shared/zoom-surface.tsx";
import type { ViewedImage } from "../../shared/attachments.tsx";
import { presentChannel, STEPS } from "./present.ts";
import type { BlockEdit } from "./doc-edit.ts";
import { downloadBlob, saveLink } from "../../shared/share.ts";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { nameOf, SHEET_FILE } from "../../shared/file-types.ts";
import { useResource } from "../../shared/use-resource.ts";

// What a sheet says of itself, before it has.
const UNTOLD = { hidden: 0, undo: false, redo: false };

// The zoom of an opened page, as a scale of its real size, and one step.
const ZOOM_MIN = 0.1;
const ZOOM_MAX = 4;
const ZOOM_STEP = 1.25;

// A page opened across the pane with what it is part of beside it: a deck's
// slides, a document's pages, a design's boards. A sheet is its grid, which
// keeps the keys and scrolls in its frame (sheet.tsx).

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
  saveAs,
  arrange,
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
  // A document's edited block, or a sheet's edited tab, saved to its file.
  save?: (edit: BlockEdit | TableEdit) => Promise<void>;
  // An Excel or CSV file's tabs, as a sheet's text, saved beside the file.
  saveAs?: (text: string) => Promise<void>;
  // A deck's slides renamed, copied or deleted as `moves` say, after which
  // the slide named `show` is the one shown; `arranging` while that runs.
  arrange?: (moves: Move[], show: string) => void;
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
  // The boards' addresses with their versions, and those as one value.
  const srcs = boards.map((board) => page(board.path));
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
  // Select picks an element to comment on; Edit, a document's, lets its
  // text be typed in place. One at a time, told to the page as each goes on
  // or off.
  const [mode, setMode] = useState<"" | "select" | "edit">("");
  const enter = (next: typeof mode) => {
    if (mode) tell(frame.current, { type: mode, on: false });
    if (next) tell(frame.current, { type: next, on: true });
    setMode(next);
    setHover(undefined);
    if (next !== "select") setPicked(undefined);
  };
  const [hover, setHover] = useState<Rect>();
  const [picked, setPicked] = useState<Pick>();
  const [comment, setComment] = useState("");
  const [tweaking, setTweaking] = useState(false);
  // Reset reloads the page: what it declares comes back.
  const [load, setLoad] = useState(0);
  // Another page: nothing picked, typed or zoomed carries over, and Edit is
  // off.
  useEffect(() => {
    setPicked(undefined);
    setHover(undefined);
    setMode((was) => (was === "edit" ? "" : was));
    setSaved("");
    setZoom(1);
    setCell(undefined);
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
  // Where a document is scrolled to inside its frame (`end`: as far as it
  // goes), which says its page, unless a step chose one. The chosen page holds
  // while the document cannot scroll to it: its last pages may share a view.
  const [where, setWhere] = useState<{
    top: number;
    end: boolean;
    chosen?: number;
  }>({ top: 0, end: false });
  // A sheet's tab, which is the pane's to say; its selection; the range a
  // comment is being written on; and what the sheet last said of itself.
  const [tab, setTab] = useState(0);
  const [cell, setCell] = useState<Cell>();
  const [ranged, setRanged] = useState<string>();
  const [told, setTold] = useState(UNTOLD);
  const formula = useRef<HTMLInputElement>(null);
  useEffect(() => {
    setWhere({ top: 0, end: false });
    setTab(0);
  }, [path]);
  // Gotos are numbered, and the document says after which one it reports
  // where it is: a report from before the latest is of a place it has left,
  // and must not undo the step that sent it on.
  const gotos = useRef(0);
  const goto = (top: number) =>
    tell(frame.current, { type: "goto", top, n: ++gotos.current });
  const shown = boards.findIndex((board) => board.path === path);
  const index = doc
    ? (where.chosen ?? pageAt(boards, where.top, where.end))
    : sheet
      ? Math.min(tab, boards.length - 1)
      : shown;
  // A document scrolls to its page and a sheet shows its tab; a deck or a
  // design shows another board, and the presenter's window with it.
  const to = (next: number) => {
    const board = boards[next];
    if (!board) return;
    if (doc) {
      setWhere((was) => ({ ...was, chosen: next }));
      goto(board.top!);
    } else if (sheet) {
      setTab(next);
      tell(frame.current, { type: "tab", index: next });
    } else if (board.path !== path) {
      go(board.path);
      channel.current?.postMessage({ at: board.path });
    }
  };
  // Past the last slide comes the first; a document ends at its ends.
  const step = (by: number) => {
    const last = boards.length - 1;
    if (index < 0) return;
    to(
      doc
        ? Math.min(last, Math.max(0, index + by))
        : (index + by + boards.length) % boards.length,
    );
  };
  // Whether the key was one that steps; Home and End go to the ends.
  const stepKey = (key: string) => {
    const by = STEPS[key];
    if (by) step(by);
    else if (key === "Home") to(0);
    else if (key === "End") to(boards.length - 1);
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
    // Not a key another widget took, nor Space on a button, which presses
    // it.
    if (
      event.defaultPrevented ||
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
  // A document's block typed in place is saved as it is left.
  const [saved, setSaved] = useState("");
  const keep = async (saving?: Promise<void>) => {
    setSaved("Saving…");
    try {
      await saving;
      setSaved("Saved");
    } catch (error) {
      setSaved(error instanceof Error ? error.message : "Not saved");
    }
  };
  const stem = nameOf(path).replace(/\.[^.]+$/, "");
  // An Excel or CSV file is only read: its page takes no change.
  const locked = sheet && SHEET_FILE.test(path);
  // What the sheet was last told to do: a file comes only as its answer.
  const asked = useRef("");
  const act = (command: string) => {
    asked.current = command;
    tell(frame.current, { type: "do", command });
  };
  // Its size as a picture: an artboard's, or a page's full height.
  const size = pageSize(info, 8000);
  const { fixed } = size;
  const sized = `&w=${size.w}&h=${size.h}`;
  const png =
    drawn &&
    info &&
    ((by: number) => `${drawn("png", path)}${sized}&scale=${by}`);
  // The page in another format: what fits its kind. Its source is Download
  // in the pane's header.
  const download = (address: string, name: string) =>
    saveLink(`${address}&download=1`, name);
  const exports: (false | undefined | [string, () => void])[] = [
    drawn
      ? ["PDF", () => download(drawn("pdf", path), `${stem}.pdf`)]
      : [
          "Print or save as PDF",
          () => window.open(`${src}#print`, "_blank", "noopener"),
        ],
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
            "image/png": fetch(png(2)).then((response) => response.blob()),
          }),
        ]),
    ],
    doc && ["Markdown", () => tell(frame.current, { type: "markdown" })],
    sheet && !/\.xlsx$/i.test(path) && ["Excel workbook", () => act("xlsx")],
    sheet && ["CSV of this tab", () => act("csv")],
  ];
  // A fixed artboard fits the stage whole; a page that names only its width
  // (an A4 document) fits it across and scrolls down inside, as a PDF does.
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
  // those of the one shown. A document is one board that counts its own.
  const [finding, setFinding] = useState(false);
  const [words, setWords] = useState("");
  const [found, setFound] = useState<number>();
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
        ? found === undefined
          ? undefined
          : [found]
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
    if (hits && boards[at].path !== path) to(at);
  };
  // Counted: the first occurrence on this board or after it.
  // biome-ignore lint/correctness/useExhaustiveDependencies: lands once per count; a step taken since must not move it
  useEffect(() => {
    const before = sum((counts ?? []).slice(0, shown));
    show(hits && before < hits ? before : 0);
  }, [query, counts]);
  // What the stage's page marks: the current occurrence, when it is there.
  const nth = boards[hit.at]?.path === path ? hit.nth : -1;
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
  ) =>
    bare ? (
      <IconButton label={label} aria-pressed={on} onClick={press}>
        {icon}
      </IconButton>
    ) : (
      <Button variant="secondary" aria-pressed={on} onClick={press}>
        {icon}
        {label}
      </Button>
    );
  const flip = (to: "select" | "edit") => enter(mode === to ? "" : to);
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
        disabled={doc && index === 0}
        onClick={() => step(-1)}
      >
        <ChevronLeft />
      </IconButton>
      <output>
        {doc && "Page "}
        {index + 1} / {boards.length}
      </output>
      <IconButton
        label={`Next ${unit.toLowerCase()}`}
        disabled={doc && index === boards.length - 1}
        onClick={() => step(1)}
      >
        <ChevronRight />
      </IconButton>
      {arrange && !presenting && (
        <SlideActions
          slides={names}
          index={index}
          title={boards[index]?.title}
          busy={arranging}
          act={rearrange}
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
          doc &&
          toggle("Edit", mode === "edit", () => flip("edit"), <Pencil />)}
        {saved && (
          <span class="small muted" role="status">
            {saved}
          </span>
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
        {png && (
          <IconButton
            label="Draw on it"
            onClick={() =>
              view({ src: png(1), name: `${stem}.png`, edit: true })
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
        <a
          class="file-button quiet icon-button"
          href={src}
          target="_blank"
          rel="noopener"
          aria-label="Open alone"
          title="Open alone"
        >
          <ExternalLink />
        </a>
      </div>
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
            <FormulaBar
              cell={cell}
              locked={locked}
              field={formula}
              write={(value) => tell(frame.current, { type: "input", value })}
            />
          )}
          <div
            class="page-stage"
            ref={stage}
            data-fixed={fixed || undefined}
            data-wide={(wide && !fixed) || undefined}
            data-presenting={presenting || undefined}
          >
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
                  src={load ? `${src}&load=${load}` : src}
                  title={info?.title || path}
                  frameRef={frame}
                  width={wide ? info!.artboard.w : undefined}
                  height={
                    fixed ? info!.artboard.h : wide ? room.h / scale : undefined
                  }
                  scale={scale}
                  onMessage={(message) => {
                    // The boards hear what fails here, and all of a document
                    // or a sheet, which has no other frame. Any other page is
                    // measured by its picture: here it is as tall as the stage.
                    if (doc || sheet || message.type === "error")
                      heard(path)(message);
                    if (message.type === "ready") {
                      setSaid(message);
                      if (mode) tell(frame.current, { type: mode, on: true });
                      // Rewritten and drawn again, a document stays where it
                      // was read, and what was found stays marked.
                      gotos.current = 0;
                      if (where.top) goto(where.top);
                      if (query) marked();
                    } else if (message.type === "drawn")
                      setSaid((was) => was && { ...was, drawn: true });
                    else if (message.type === "zoom") {
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
                    else if (message.type === "at") {
                      if (message.after !== gotos.current) return;
                      setWhere((was) => ({
                        top: message.top,
                        end: message.end,
                        chosen:
                          message.end &&
                          (was.chosen ?? -1) >=
                            pageAt(boards, message.top, false)
                            ? was.chosen
                            : undefined,
                      }));
                    } else if (message.type === "find") {
                      // A slide's are counted in its source, not here.
                      if (doc) setFound(message.count);
                    } else if (message.type === "edit") {
                      if ("table" in message) {
                        const [undo, redo] = [!!message.undo, !!message.redo];
                        setTold((was) => ({ ...was, undo, redo }));
                      }
                      void keep(save?.(message));
                    } else if (message.type === "file") {
                      // The page is untrusted: only what was asked of a
                      // sheet is taken. Its text is asked of a read file alone.
                      const made =
                        sheet && message.as === asked.current
                          ? fileOf(message)
                          : undefined;
                      asked.current = "";
                      if (typeof made === "string") setSaved(made);
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
                      // Drawn again (rewritten), it shows the tab it showed.
                      const kept = Math.min(tab, boards.length - 1);
                      if (message.active !== kept)
                        tell(frame.current, { type: "tab", index: kept });
                      const hidden = Number(message.hidden) || 0;
                      setTold((was) => ({ ...was, hidden }));
                    } else if (message.type === "cell")
                      setCell(cellOf(message));
                    else if (message.type === "refused")
                      setSaved(String(message.message).slice(0, 300));
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
                {doc &&
                  boards.map((board, number) => {
                    // Where the page starts in the stage, while that shows.
                    const top = (board.top! - where.top) * scale;
                    return (
                      number > 0 &&
                      top > 0 &&
                      top < room.h && (
                        <span
                          key={board.top}
                          class="page-break small"
                          style={{ top: `${top}px` }}
                        >
                          Page {number + 1}
                        </span>
                      )
                    );
                  })}
                {mode === "select" && hover && mark(hover, "hover")}
                {picked && mark(picked.rect, "picked")}
              </div>
            </div>
            {!presenting && counter}
            {presenting && notesShown && info?.notes && (
              <p class="page-notes">{info.notes}</p>
            )}
            {presenting && (
              <div class="page-present-bar">
                {counter}
                <Button
                  variant="secondary"
                  disabled={!info?.notes}
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
                        boards.map((board) => board.path),
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
          {!presenting && info?.notes && (
            <p class="page-notes">
              <span class="muted">Notes</span>
              {info.notes}
            </p>
          )}
          {sheet && (
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
