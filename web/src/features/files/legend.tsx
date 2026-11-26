import type { RefObject } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { EyeOff, TriangleAlert } from "lucide-preact";
import { Button } from "../../shared/ui.tsx";
import Frame, { type PageMessage } from "../../shared/page-frame.tsx";
import { TAP_SLOP_PX } from "../../shared/zoom.ts";
import type { Board } from "./boards.ts";

// How wide a board is pictured: beside the stage, and as a grid in its place.
const LEGEND_WIDTH = 128;
const GRID_WIDTH = 176;

// The current one of a list stays in view, and keeps the focus the list has.
export function useCurrentInView(
  list: RefObject<HTMLElement | null>,
  index: number,
  grid = false,
) {
  // biome-ignore lint/correctness/useExhaustiveDependencies: whenever another is current
  useEffect(() => {
    const pane = list.current;
    const current = pane?.querySelector<HTMLElement>('[aria-selected="true"]');
    if (!pane || !current) return;

    const box = current.getBoundingClientRect();
    const view = pane.getBoundingClientRect();
    const left = view.left + pane.clientLeft;
    const top = view.top + pane.clientTop;
    const right = left + pane.clientWidth;
    const bottom = top + pane.clientHeight;
    // A click already reached this item, even if only its edge is visible.
    if (
      document.activeElement === current &&
      box.right > left &&
      box.left < right &&
      box.bottom > top &&
      box.top < bottom
    )
      return;

    // Reveal keyboard and toolbar selections inside this list alone.
    pane.scrollBy(
      Math.min(0, box.left - left) || Math.max(0, box.right - right),
      Math.min(0, box.top - top) || Math.max(0, box.bottom - bottom),
    );
    if (pane.contains(document.activeElement))
      current.focus({ preventScroll: true });
  }, [index, grid]);
}

// The sequence small beside the stage (a filmstrip under it in a narrow pane).
// Only boards near the legend's view are drawn.
export default function Legend({
  unit,
  boards,
  srcs,
  stamp,
  index,
  heard,
  to,
  move,
  menu,
  grid,
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
  // Laid out in rows over the whole body.
  grid?: boolean;
}) {
  const list = useRef<HTMLDivElement>(null);
  // The boards near the legend's view, by their place.
  const [near, setNear] = useState<ReadonlySet<number>>(new Set());
  // Watched again when the boards are others.
  useEffect(() => {
    const items = list.current!.children;
    const observer = new IntersectionObserver(
      (entries) =>
        setNear((was) => {
          const next = new Set(was);
          for (const entry of entries) {
            const at = [...items].indexOf(entry.target);
            if (entry.isIntersecting) next.add(at);
            else next.delete(at);
          }
          return next;
        }),
      { root: list.current, rootMargin: "100%" },
    );
    for (const item of items) observer.observe(item);
    return () => observer.disconnect();
  }, [stamp]);
  useCurrentInView(list, index, grid);
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
  // down the legend or, in a filmstrip or a grid, across its rows.
  const gapAt = ({ clientX, clientY }: PointerEvent) => {
    const all = items();
    const across = all[1]?.offsetLeft > all[0].offsetLeft;
    // In a row, though the pointer has left them.
    const y = Math.min(
      Math.max(clientY, all[0].getBoundingClientRect().top),
      all.at(-1)!.getBoundingClientRect().bottom - 1,
    );
    return all.filter((item) => {
      const box = item.getBoundingClientRect();
      return across
        ? box.bottom <= y ||
            (box.top <= y && clientX > box.left + box.width / 2)
        : clientY > box.top + box.height / 2;
    }).length;
  };
  return (
    <div
      class={grid ? "page-legend grid" : "page-legend"}
      role="listbox"
      aria-label={`${unit}s`}
      ref={list}
      // Up and Down walk it as a list; the pane's keys do the rest.
      onKeyDown={(event) => {
        const by = {
          ArrowDown: 1,
          ArrowUp: -1,
          ...(grid && { ArrowRight: 1, ArrowLeft: -1 }),
        }[event.key];
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
        // The press ends once the click its release makes has passed.
        setTimeout(() => {
          if (press.current === held) press.current = undefined;
        });
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
      {boards.map((board, at) => (
        <div
          // By its place: a slide renamed is the same item, so the legend
          // keeps its scroll through a reorder.
          key={at}
          class="page-legend-item"
          data-path={board.path}
          data-failed={board.failed || undefined}
          data-issue={board.issue || undefined}
          data-skipped={board.skip}
          data-dragging={drop?.from === at || undefined}
          data-drop={
            drop?.gap === at
              ? "before"
              : drop?.gap === at + 1 && at === boards.length - 1
                ? "after"
                : undefined
          }
        >
          {board.section && (
            <span class="page-legend-section small truncate">
              {board.section}
            </span>
          )}
          <Frame
            live={false}
            src={srcs[at]}
            title={`${unit} ${at + 1}: ${board.title}`}
            width={board.w}
            height={board.h}
            scale={(grid ? GRID_WIDTH : LEGEND_WIDTH) / board.w}
            far={!near.has(at)}
            // A document's pages are counted on the stage: a thumbnail drawn
            // anew starts at one.
            onMessage={(message) =>
              message.type !== "pages" && heard(board.path)(message)
            }
          />
          <span class="page-legend-name small truncate">
            {(board.failed || board.issue) && <TriangleAlert />}
            {board.skip && <EyeOff />}
            {`${at + 1} · ${board.title}`}
          </span>
          <Button
            variant="quiet"
            class="cover"
            role="option"
            aria-label={`${board.section ? `${board.section}, ` : ""}${unit} ${at + 1}: ${board.title}${board.failed ? ", failed to run" : ""}${board.issue ? `, ${board.issue}` : ""}${board.skip ? ", skipped" : ""}`}
            aria-selected={at === index}
            tabIndex={at === index ? 0 : -1}
            onClick={(event) => {
              event.currentTarget.focus({ preventScroll: true });
              to(at);
            }}
          />
        </div>
      ))}
    </div>
  );
}
