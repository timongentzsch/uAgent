import { useRef } from "preact/hooks";
import { Button } from "../../shared/ui.tsx";
import type { Board } from "./boards.ts";
import { useCurrentInView } from "./legend.tsx";

// A sheet's chrome around its grid, which the page draws itself: above it
// the cell that was clicked, under it the tabs.

export interface Cell {
  ref: string;
  raw: string;
}

// Where the clicked cell is and what was typed in it; empty until one is.
export function FormulaBar({ cell }: { cell?: Cell }) {
  return (
    <div class="sheet-formula small" role="group" aria-label="Formula bar">
      <output>{cell?.ref}</output>
      <span class="truncate">{cell?.raw}</span>
    </div>
  );
}

// A sheet's legend: its tabs in a row, the one shown selected and kept in
// view. Left and Right walk them, as in any tab list.
export function SheetTabs({
  boards,
  index,
  to,
}: {
  boards: Board[];
  index: number;
  to: (index: number) => void;
}) {
  const list = useRef<HTMLDivElement>(null);
  useCurrentInView(list, index);
  return (
    <div
      class="tabs sheet-tabs"
      role="tablist"
      aria-label="Tabs"
      ref={list}
      onKeyDown={(event) => {
        const by = { ArrowLeft: -1, ArrowRight: 1 }[event.key];
        if (!by || !boards[index + by]) return;
        to(index + by);
        event.preventDefault();
      }}
    >
      {boards.map((board, at) => (
        <Button
          key={at}
          size="compact"
          role="tab"
          aria-selected={at === index}
          tabIndex={at === index ? 0 : -1}
          onClick={() => to(at)}
        >
          {board.title}
        </Button>
      ))}
    </div>
  );
}
