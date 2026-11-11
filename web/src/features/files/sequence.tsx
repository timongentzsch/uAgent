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
  TriangleAlert,
} from "lucide-preact";
import { Button, ConfirmModal, IconButton, Switch } from "../../shared/ui.tsx";
import { Input, Textarea } from "../../shared/form-controls.tsx";
import Frame, {
  BoardFrame,
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
  duplicated,
  type Kind,
  type Move,
  moved,
  pageAt,
  removed,
  spotAt,
  sum,
} from "./boards.ts";
import { dirOf } from "./flow.ts";
import { readTokens } from "./tokens.ts";
import { bindZoomWheel, TAP_SLOP_PX } from "../../shared/zoom.ts";
import { useLatest } from "../../shared/use-latest.ts";
import { ZoomControls } from "../../shared/zoom-surface.tsx";
import type { ViewedImage } from "../../shared/attachments.tsx";
import { presentChannel, STEPS } from "./present.ts";
import type { BlockEdit } from "./doc-edit.ts";
import { downloadBlob, saveLink } from "../../shared/share.ts";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { nameOf } from "../../shared/file-types.ts";
import { useResource } from "../../shared/use-resource.ts";

// The zoom of an opened page, as a scale of its real size, and one step.
const ZOOM_MIN = 0.1;
const ZOOM_MAX = 4;
const ZOOM_STEP = 1.25;
// How wide a board is drawn in the legend.
const LEGEND_WIDTH = 128;

// A page opened across the pane with what it is part of beside it: a deck's
// slides, a document's pages, a design's boards. The stage shows one: a
// fixed artboard scaled to fit, a page at the pane's own width. The legend,
// the counter and the keys step through the rest; Find looks through all of
// it. Select picks an element to comment on; Tweaks tries the levers the
// page declares; Present shows it over the whole app (full screen where the
// browser allows) with a slide's notes; Open and PDF show it alone in a tab.

interface Tweak {
  editor?: string;
  default?: unknown;
  options?: unknown[];
  min?: number;
  max?: number;
  step?: number;
  // Shown beside the value, never applied: the page's CSS gives the unit.
  unit?: string;
}

// The lines it adds to the draft, which the artifact skill reads.
const commentLine = (path: string, pick: Pick, comment: string) =>
  `\`${path}\` \`${pick.selector}\`${pick.text ? ` “${pick.text}”` : ""} — ${comment}`;
const pairs = (values: Record<string, unknown>) =>
  Object.entries(values)
    .map(([name, value]) => `${name} ${value}`)
    .join(", ");

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

// The sequence small beside the stage (under it, as a filmstrip, in a narrow
// pane): each board numbered with its title, the current one marked and kept
// in view. A document is one frame with its pages marked on it, since a
// frame per page would each draw the whole document. Only the boards near
// the legend's view are drawn.
function Legend({
  unit,
  boards,
  srcs,
  stamp,
  index,
  heard,
  to,
  move,
  menu,
}: {
  // "Slide" or "Page": what a board is called.
  unit: string;
  boards: Board[];
  // The boards' addresses, and those as one value: other boards, or one
  // rewritten, are another.
  srcs: string[];
  stamp: string;
  index: number;
  heard: (path: string) => (message: PageMessage) => void;
  to: (index: number) => void;
  // A deck's alone: takes a slide to a new place, and opens a slide's menu.
  move?: (from: number, to: number) => void;
  menu?: (index: number) => void;
}) {
  const list = useRef<HTMLDivElement>(null);
  const [near, setNear] = useState<ReadonlySet<string>>(new Set());
  // Watched again when the boards are others.
  useEffect(() => {
    const observer = new IntersectionObserver(
      (entries) =>
        setNear((was) => {
          const next = new Set(was);
          for (const entry of entries) {
            const path = (entry.target as HTMLElement).dataset.path!;
            if (entry.isIntersecting) next.add(path);
            else next.delete(path);
          }
          return next;
        }),
      { root: list.current, rootMargin: "100%" },
    );
    for (const item of list.current!.querySelectorAll("[data-path]"))
      observer.observe(item);
    return () => observer.disconnect();
  }, [stamp]);
  // The current one stays in view, and keeps the focus the legend has.
  useEffect(() => {
    const current = list.current?.querySelector<HTMLElement>(
      '[aria-selected="true"]',
    );
    current?.scrollIntoView({ block: "nearest", inline: "nearest" });
    if (list.current?.contains(document.activeElement))
      current?.focus({ preventScroll: true });
  }, [index]);
  const option = (at: number, label: string) => ({
    role: "option" as const,
    "aria-selected": at === index,
    "aria-label": label,
    tabIndex: at === index ? 0 : -1,
    onClick: () => to(at),
  });
  // A slide dragged to a new place: the press, held once it has moved, and
  // the gap it would drop into (0 is before the first). A press that moved
  // opens nothing.
  const press = useRef<{ from: number; x: number; y: number; moved?: true }>(
    undefined,
  );
  const [drop, setDrop] = useState<{ from: number; gap: number }>();
  const items = () => [
    ...list.current!.querySelectorAll<HTMLElement>("[data-path]"),
  ];
  const itemAt = (event: Event) =>
    items().indexOf(
      (event.target as Element).closest<HTMLElement>("[data-path]")!,
    );
  // The gap under the pointer: past the middles of how many slides it is,
  // down the legend or, in a filmstrip, across it.
  const gapAt = ({ clientX, clientY }: PointerEvent) => {
    const all = items();
    const across = all[1]?.offsetLeft > all[0].offsetLeft;
    return all.filter((item) => {
      const box = item.getBoundingClientRect();
      return across
        ? clientX > box.left + box.width / 2
        : clientY > box.top + box.height / 2;
    }).length;
  };
  // Only a document's pages say where in it they start.
  const doc = boards[0].top !== undefined && boards[0];
  const scale = LEGEND_WIDTH / boards[0].w;
  return (
    <div
      class="page-legend"
      role="listbox"
      aria-label={`${unit}s`}
      data-doc={!!doc || undefined}
      ref={list}
      // Up and Down walk it as a list; the pane's keys do the rest.
      onKeyDown={(event) => {
        const by = { ArrowDown: 1, ArrowUp: -1 }[event.key];
        if (!by || !boards[index + by]) return;
        to(index + by);
        event.preventDefault();
      }}
      onPointerDown={(event) => {
        const from = itemAt(event);
        // A touch that drags scrolls the legend.
        press.current =
          move && from >= 0 && !event.button && event.pointerType !== "touch"
            ? { from, x: event.clientX, y: event.clientY }
            : undefined;
      }}
      onPointerMove={(event) => {
        const held = press.current;
        if (!held) return;
        const far = Math.hypot(event.clientX - held.x, event.clientY - held.y);
        if (!held.moved && far < TAP_SLOP_PX) return;
        held.moved = true;
        event.currentTarget.setPointerCapture(event.pointerId);
        const gap = gapAt(event);
        setDrop((was) => (was?.gap === gap ? was : { from: held.from, gap }));
      }}
      onPointerUp={(event) => {
        const held = press.current;
        setDrop(undefined);
        if (!held?.moved) return;
        const gap = gapAt(event);
        const place = gap > held.from ? gap - 1 : gap;
        if (place !== held.from) move?.(held.from, place);
      }}
      onPointerCancel={() => {
        press.current = undefined;
        setDrop(undefined);
      }}
      onClickCapture={(event) => {
        if (press.current?.moved) event.stopPropagation();
      }}
      onContextMenu={(event) => {
        const at = itemAt(event);
        if (!menu || at < 0) return;
        event.preventDefault();
        // Under a press (where the menu comes with the button going down)
        // it opens once the button is up and that release is dealt with:
        // a popover opened before is dismissed by it.
        if (event.buttons)
          addEventListener("pointerup", () => setTimeout(() => menu(at)), {
            once: true,
          });
        else menu(at);
      }}
    >
      {doc ? (
        <div class="page-legend-doc">
          <BoardFrame
            src={srcs[0]}
            title={`${doc.title}: pages`}
            w={doc.w}
            h={sum(boards.map((board) => board.h))}
            scale={scale}
          />
          {boards.map((board, at) => (
            <Button
              key={board.top}
              variant="quiet"
              class="cover page-legend-page small"
              style={{
                top: `${board.top! * scale}px`,
                height: `${board.h * scale}px`,
              }}
              {...option(at, `Page ${at + 1}`)}
            >
              <span>{at + 1}</span>
            </Button>
          ))}
        </div>
      ) : (
        boards.map((board, at) => (
          <div
            // By its place: a slide renamed is the same item, so the legend
            // keeps its scroll through a reorder.
            key={at}
            class="page-legend-item"
            data-path={board.path}
            data-failed={board.failed || undefined}
            data-dragging={drop?.from === at || undefined}
            data-drop={
              drop?.gap === at
                ? "before"
                : drop?.gap === at + 1 && at === boards.length - 1
                  ? "after"
                  : undefined
            }
          >
            <BoardFrame
              src={srcs[at]}
              title={`${unit} ${at + 1}: ${board.title}`}
              w={board.w}
              h={board.h}
              scale={LEGEND_WIDTH / board.w}
              far={!near.has(board.path)}
              onMessage={heard(board.path)}
            />
            <span class="page-legend-name small truncate">
              {board.failed && <TriangleAlert />}
              {`${at + 1} · ${board.title}`}
            </span>
            <Button
              variant="quiet"
              class="cover"
              {...option(
                at,
                `${unit} ${at + 1}: ${board.title}${board.failed ? ", failed to run" : ""}`,
              )}
            />
          </div>
        ))
      )}
    </div>
  );
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
  picture,
  pdf,
  deckPdf,
  presenter,
  save,
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
  // The page drawn as a PNG by the agent's browser, where there is one.
  picture?: (width: number, height: number, scale: number) => string;
  // The page printed as a PDF on its paper by the agent's browser, and a
  // deck's slides as one, a page each at a slide's size.
  pdf?: string;
  deckPdf?: (width: number, height: number) => string;
  // Opens the presenter's view of a deck at a slide, in a window.
  presenter?: (slides: string[], at: string) => void;
  // A document's edited block saved to its file.
  save?: (edit: BlockEdit) => Promise<void>;
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
  const unit = kind === "deck" ? "Slide" : "Page";
  const src = page(path);
  // The boards' addresses with their versions, and those as one value.
  const srcs = boards.map((board) => page(board.path));
  const stamp = srcs.join("\n");
  const frame = useRef<HTMLIFrameElement>(null);
  const stage = useRef<HTMLDivElement>(null);
  const scroller = useRef<HTMLDivElement>(null);
  const [room, setRoom] = useState({ w: 0, h: 0 });
  // What the page said it is, for the address it said it at.
  const [said, setSaid] = useState<PageInfo & { src: string }>();
  const info = said?.src === src ? said : undefined;
  const [selecting, setSelecting] = useState(false);
  const [hover, setHover] = useState<Rect>();
  const [picked, setPicked] = useState<Pick>();
  const [comment, setComment] = useState("");
  const [tweaking, setTweaking] = useState(false);
  const [values, setValues] = useState<Record<string, unknown>>({});
  // Reset reloads the page: what it declares comes back.
  const [load, setLoad] = useState(0);
  useEffect(() => {
    const observer = new ResizeObserver(([entry]) =>
      setRoom({ w: entry.contentRect.width, h: entry.contentRect.height }),
    );
    if (scroller.current) observer.observe(scroller.current);
    return () => observer.disconnect();
  }, []);
  // Another page: nothing picked or tried carries over.
  useEffect(() => {
    setPicked(undefined);
    setHover(undefined);
    setValues({});
  }, [src]);
  // Presenting: over the app, full screen where the browser allows, the
  // notes shown on a phone held upright or with N. Leaving full screen (a
  // presenter's window opening takes it away) keeps presenting; End or Esc
  // stop.
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
  // goes), which says its page, unless a step chose one. The chosen page
  // shows at once and holds while the document cannot scroll to it: its last
  // pages may all lie in its last view.
  const [where, setWhere] = useState<{
    top: number;
    end: boolean;
    chosen?: number;
  }>({ top: 0, end: false });
  useEffect(() => setWhere({ top: 0, end: false }), [path]);
  // Gotos are numbered, and the document says after which one it reports
  // where it is: a report from before the latest is of a place it has left,
  // and must not undo the step that sent it on.
  const gotos = useRef(0);
  const goto = (top: number) =>
    tell(frame.current, { type: "goto", top, n: ++gotos.current });
  const shown = boards.findIndex((board) => board.path === path);
  const index = doc
    ? (where.chosen ?? pageAt(boards, where.top, where.end))
    : shown;
  // A document scrolls to its page; a deck or a design shows another board,
  // and the presenter's window with it.
  const to = (next: number) => {
    const board = boards[next];
    if (!board) return;
    if (doc) {
      setWhere((was) => ({ ...was, chosen: next }));
      goto(board.top!);
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
  const presentKey = (key: string) => {
    stepKey(key);
    if (key === "n" || key === "N") setNotesShown((shown) => !shown);
    if (key === "Escape") present(false);
  };
  // The keys step through the sequence outside presenting too, never in a
  // field. The arrows do from anywhere; the paging keys only while the pane
  // has them (the transcript pages with the same keys). Read as they are
  // now: a key pressed right after a step must count from where that step
  // led, not from the render its listener was bound in.
  const keys = useLatest((event: KeyboardEvent) => {
    const target = event.target as Element;
    // Not a key another widget took, nor Space on a button, which presses
    // it.
    if (
      event.defaultPrevented ||
      (event.key === " " && target.closest?.("button"))
    )
      return;
    if (presenting) presentKey(event.key);
    else if (
      boards.length > 1 &&
      !event.altKey &&
      !event.metaKey &&
      !event.ctrlKey &&
      !target.closest?.("input, textarea, select, [contenteditable]") &&
      (event.key.startsWith("Arrow") ||
        target === document.body ||
        target.closest?.(".files")) &&
      stepKey(event.key)
    )
      event.preventDefault();
  });
  useEffect(() => {
    const pressed = (event: KeyboardEvent) => keys.current(event);
    document.addEventListener("keydown", pressed);
    return () => document.removeEventListener("keydown", pressed);
  }, [keys]);
  // A document: Edit lets its text be typed in place, each block saved as
  // it is left; its page breaks are marked where print will put them.
  const [editing, setEditing] = useState(false);
  const [saved, setSaved] = useState("");
  const edit = (on: boolean) => {
    setEditing(on);
    if (on) select(false);
    tell(frame.current, { type: "edit", on });
  };
  const keep = async (change: BlockEdit) => {
    setSaved("Saving…");
    try {
      await save?.(change);
      setSaved("Saved");
    } catch (error) {
      setSaved(error instanceof Error ? error.message : "Not saved");
    }
  };
  useEffect(() => {
    setEditing(false);
    setSaved("");
  }, [src]);
  // Its exports' name: the file's, without .html.
  const stem = nameOf(path).replace(/\.html?$/i, "");
  // Its size as a picture: an artboard's, or a page's full height.
  const size = pageSize(info, 8000);
  const { fixed } = size;
  // A fixed artboard fits the stage whole; a page that names only its width
  // (an A4 document) fits it across and scrolls down inside, as a PDF does.
  const wide = !!info?.artboard.w;
  const fit = !room.w
    ? 1
    : fixed
      ? Math.min(1, room.w / info!.artboard.w, room.h / info!.artboard.h)
      : wide
        ? Math.min(1, room.w / info!.artboard.w)
        : 1;
  // How far the person zoomed from the fit; each page opens at the fit.
  const [zoom, setZoom] = useState(1);
  useEffect(() => setZoom(1), [src]);
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
  const select = (on: boolean) => {
    if (on && editing) {
      setEditing(false);
      tell(frame.current, { type: "edit", on: false });
    }
    setSelecting(on);
    setHover(undefined);
    if (!on) setPicked(undefined);
    tell(frame.current, { type: "select", on });
  };
  // The design system the page links, by its tokens.css.
  const sheet = info?.sheets?.find((href) => /\/tokens\.css(\?|$)/.test(href));
  const system = sheet
    ? decodeURIComponent(new URL(sheet).pathname.slice(base.length))
    : "";
  const theme = useResource(
    () => (system ? read(system).then(readTokens) : undefined),
    [system],
  );
  const [themed, setThemed] = useState<Record<string, string>>({});
  const tryTheme = (name: string, value: string) => {
    setThemed((all) => ({ ...all, [name]: value }));
    tell(frame.current, { type: "tweak", name, value });
  };
  const declared = Object.entries(
    (info?.tweaks ?? {}) as Record<string, Tweak>,
  );
  const set = (name: string, value: unknown) => {
    setValues((all) => ({ ...all, [name]: value }));
    tell(frame.current, { type: "tweak", name, value });
  };
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
  // The occurrence `next` becomes the current one, on its board.
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
  const [legend, setLegend] = useState(true);
  const whole = doc ? "document" : kind === "deck" ? "deck" : "design";
  // A deck rearranged from here: the stage follows the slide it shows to
  // its new name, or to a neighbour when that slide is the one deleted.
  const names = boards.map((board) => nameOf(board.path));
  const rearrange = (moves: Move[]) => {
    const own = moves.find((move) => move.from === names[index] && !move.copy);
    if (moves.length)
      arrange?.(
        moves,
        (own ? own.to : names[index]) ||
          names[index + 1] ||
          names[index - 1] ||
          "",
      );
  };
  const [deleting, setDeleting] = useState(false);
  const stepper = useRef<HTMLDivElement>(null);
  // Which board of how many, with the previous and the next.
  const counter = (boards.length > 1 || arrange) && (
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
      {/* Beside the legend, not in it: a listbox holds only its options. */}
      {arrange && !presenting && (
        <Menu label="Slide actions">
          <MenuItem
            disabled={arranging || !index}
            onClick={() => rearrange(moved(names, index, index - 1))}
          >
            Move up
          </MenuItem>
          <MenuItem
            disabled={arranging || index === boards.length - 1}
            onClick={() => rearrange(moved(names, index, index + 1))}
          >
            Move down
          </MenuItem>
          <MenuItem
            disabled={arranging}
            onClick={() => rearrange(duplicated(names, index))}
          >
            Duplicate
          </MenuItem>
          <MenuItem disabled={arranging} onClick={() => setDeleting(true)}>
            Delete…
          </MenuItem>
        </Menu>
      )}
    </div>
  );
  return (
    <div class="open-page">
      <div class="page-tools">
        {boards.length > 1 && (
          <IconButton
            label="Legend"
            aria-pressed={legend}
            onClick={() => setLegend(!legend)}
          >
            <PanelLeft />
          </IconButton>
        )}
        <Button
          variant="secondary"
          aria-pressed={selecting}
          onClick={() => select(!selecting)}
        >
          <MousePointerClick />
          Select
        </Button>
        {save && doc && (
          <Button
            variant="secondary"
            aria-pressed={editing}
            onClick={() => edit(!editing)}
          >
            <Pencil />
            Edit
          </Button>
        )}
        {saved && (
          <span class="small muted" role="status">
            {saved}
          </span>
        )}
        {(declared.length > 0 || !!theme.value?.colors.length) && (
          <Button
            variant="secondary"
            aria-pressed={tweaking}
            onClick={() => setTweaking(!tweaking)}
          >
            <SlidersHorizontal />
            Tweaks
          </Button>
        )}
        <IconButton
          label="Find"
          aria-pressed={finding}
          onClick={() => setFinding(!finding)}
        >
          <Search />
        </IconButton>
        <IconButton label="Present" onClick={() => present(true)}>
          <Presentation />
        </IconButton>
        {picture && info && (
          <IconButton
            label="Draw on it"
            onClick={() =>
              view({
                src: picture(size.w, size.h, 1),
                name: `${stem}.png`,
                edit: true,
              })
            }
          >
            <PenLine />
          </IconButton>
        )}
        {/* The page in another format: what fits its kind. Its source is
            Download in the pane's header. */}
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
          {pdf ? (
            <MenuItem
              onClick={() => saveLink(`${pdf}&download=1`, `${stem}.pdf`)}
            >
              PDF
            </MenuItem>
          ) : (
            <MenuItem
              onClick={() => window.open(`${src}#print`, "_blank", "noopener")}
            >
              Print or save as PDF
            </MenuItem>
          )}
          {deckPdf && (
            <MenuItem
              onClick={() =>
                saveLink(
                  `${deckPdf(size.w, size.h)}&download=1`,
                  `${nameOf(dirOf(path)) || "deck"}.pdf`,
                )
              }
            >
              PDF, a page per slide
            </MenuItem>
          )}
          {picture && info && (
            <>
              <MenuItem
                onClick={() =>
                  saveLink(
                    `${picture(size.w, size.h, 2)}&download=1`,
                    `${stem}.png`,
                  )
                }
              >
                PNG image
              </MenuItem>
              <MenuItem
                onClick={() =>
                  navigator.clipboard.write([
                    new ClipboardItem({
                      "image/png": fetch(picture(size.w, size.h, 2)).then(
                        (response) => response.blob(),
                      ),
                    }),
                  ])
                }
              >
                Copy as image
              </MenuItem>
            </>
          )}
          {doc && (
            <MenuItem onClick={() => tell(frame.current, { type: "markdown" })}>
              Markdown
            </MenuItem>
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
        {legend && boards.length > 1 && (
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
                // A new page fades in once it has drawn. (No effect hides it
                // again: a page that loads fast is ready before one runs.)
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
                  // A frame an address: what a page says as it is left (an
                  // error while it unloads) has no window here to be taken
                  // for the next page's.
                  key={`${load} ${src}`}
                  src={src}
                  title={info?.title || path}
                  frameRef={frame}
                  width={wide ? info!.artboard.w : undefined}
                  height={
                    fixed ? info!.artboard.h : wide ? room.h / scale : undefined
                  }
                  scale={scale}
                  onMessage={(message) => {
                    // The boards hear what fails here, and all of a
                    // document, which has no other frame that says it. Any
                    // other page is measured by its picture: a page as
                    // tall as it reads says the stage's height here.
                    if (doc || message.type === "error") heard(path)(message);
                    if (message.type === "ready") {
                      setSaid({ ...message, src });
                      if (editing)
                        tell(frame.current, { type: "edit", on: true });
                      if (selecting)
                        tell(frame.current, { type: "select", on: true });
                      // Rewritten and drawn again, a document stays where it
                      // was read, and what was found stays marked.
                      gotos.current = 0;
                      if (where.top) goto(where.top);
                      if (query) marked();
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
                      // A page that grows (its fonts, its images) is pictured whole.
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
                    } else if (message.type === "edit") void keep(message);
                    else if (message.type === "markdown")
                      downloadBlob(
                        new Blob([message.text], { type: "text/markdown" }),
                        `${stem}.md`,
                      );
                    else if (message.type === "hover") setHover(message.rect);
                    else if (message.type === "pick") setPicked(message);
                    else if (message.type === "picked")
                      setPicked((was) => was && { ...was, rect: message.rect });
                    else if (message.type === "key") {
                      if (presenting) presentKey(message.key);
                      else if (message.key === "Escape") select(false);
                      else if (!selecting) stepKey(message.key);
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
                {selecting && hover && mark(hover, "hover")}
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
                {info?.notes && (
                  <Button
                    variant="secondary"
                    aria-pressed={notesShown}
                    onClick={() => setNotesShown(!notesShown)}
                  >
                    Notes
                  </Button>
                )}
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
        </div>
        {tweaking && (
          <aside class="page-tweaks" aria-label="Tweaks">
            {declared.map(([name, tweak]) => {
              const value = values[name] ?? tweak.default;
              const label = name.replace(/^--/, "");
              return (
                <div key={name} class="page-tweak">
                  <span class="small">
                    {label}
                    {tweak.editor !== "color" && tweak.editor !== "boolean" && (
                      <span class="muted">{` ${value ?? ""}${tweak.unit ?? ""}`}</span>
                    )}
                  </span>
                  {tweak.editor === "color" ? (
                    <div class="segmented">
                      {(tweak.options ?? []).map((option) => (
                        <Button
                          key={String(option)}
                          variant="secondary"
                          class="page-swatch"
                          aria-label={String(option)}
                          aria-pressed={value === option}
                          style={{ background: String(option) }}
                          onClick={() => set(name, option)}
                        />
                      ))}
                      <Input
                        type="color"
                        aria-label={`${label}, any colour`}
                        value={String(value ?? "")}
                        onInput={(event) =>
                          set(name, event.currentTarget.value)
                        }
                      />
                    </div>
                  ) : tweak.editor === "enum" ? (
                    <div class="segmented">
                      {(tweak.options ?? []).map((option) => (
                        <Button
                          key={String(option)}
                          variant="secondary"
                          aria-pressed={value === option}
                          onClick={() => set(name, option)}
                        >
                          {String(option)}
                        </Button>
                      ))}
                    </div>
                  ) : tweak.editor === "boolean" ? (
                    <Switch
                      label={label}
                      checked={!!value}
                      onChange={(checked) => set(name, checked)}
                    />
                  ) : (
                    <Input
                      type="range"
                      aria-label={label}
                      min={tweak.min ?? 0}
                      max={tweak.max ?? 100}
                      step={tweak.step ?? (tweak.editor === "int" ? 1 : 0.01)}
                      value={Number(value ?? 0)}
                      onInput={(event) =>
                        set(name, Number(event.currentTarget.value))
                      }
                    />
                  )}
                </div>
              );
            })}
            {theme.value && theme.value.colors.length > 0 && (
              <section class="page-theme" aria-label="Theme">
                <span class="small">
                  Theme <span class="muted">{system}</span>
                </span>
                {theme.value.themes.length > 1 && (
                  <div class="segmented">
                    {theme.value.themes.map((id) => (
                      <Button
                        key={id}
                        variant="secondary"
                        aria-pressed={
                          (themed.theme ?? theme.value!.themes[0]) === id
                        }
                        onClick={() => tryTheme("theme", id)}
                      >
                        {id}
                      </Button>
                    ))}
                  </div>
                )}
                {theme.value.colors
                  .filter((color) => /^#[0-9a-f]{6}$/i.test(color.value))
                  .map((color) => (
                    <label key={color.name} class="page-theme-token small">
                      <Input
                        type="color"
                        value={themed[`--${color.name}`] ?? color.value}
                        onInput={(event) =>
                          tryTheme(`--${color.name}`, event.currentTarget.value)
                        }
                      />
                      {color.name}
                    </label>
                  ))}
                {addToChat && Object.keys(themed).length > 0 && (
                  <Button
                    onClick={() => {
                      const { theme: id, ...colors } = themed;
                      addToChat(
                        `\`${system}\` theme ${id ?? theme.value!.themes[0]}: ${pairs(colors)}`,
                      );
                    }}
                  >
                    Keep theme
                  </Button>
                )}
              </section>
            )}
            <div class="page-tweak-actions">
              <Button
                variant="secondary"
                onClick={() => {
                  setThemed({});
                  setValues({});
                  setLoad(load + 1);
                }}
              >
                Reset
              </Button>
              {addToChat && (
                <Button
                  disabled={!Object.keys(values).length}
                  onClick={() =>
                    addToChat(`\`${path}\` tweaks: ${pairs(values)}`)
                  }
                >
                  Keep
                </Button>
              )}
            </div>
          </aside>
        )}
      </div>
      {deleting && (
        <ConfirmModal
          title="Delete slide"
          close={() => setDeleting(false)}
          confirm={() => {
            setDeleting(false);
            rearrange(removed(names, index));
          }}
        >
          {`${boards[index].title} (${names[index]}) is deleted from the folder. This cannot be undone.`}
        </ConfirmModal>
      )}
      {picked && addToChat && (
        <form
          class="page-comment"
          onSubmit={(event) => {
            event.preventDefault();
            if (!comment.trim()) return;
            addToChat(commentLine(path, picked, comment.trim()), true);
            setComment("");
            setPicked(undefined);
          }}
        >
          <span class="small muted">
            {`Comment on ${picked.tag}${picked.text ? ` “${picked.text}”` : ""}`}
          </span>
          <Textarea
            aria-label="Comment"
            rows={2}
            value={comment}
            onInput={(event) => setComment(event.currentTarget.value)}
          />
          <div class="page-tweak-actions">
            <Button
              variant="secondary"
              type="button"
              onClick={() => setPicked(undefined)}
            >
              Cancel
            </Button>
            <Button type="submit" disabled={!comment.trim()}>
              Add to chat
            </Button>
          </div>
        </form>
      )}
    </div>
  );
}
