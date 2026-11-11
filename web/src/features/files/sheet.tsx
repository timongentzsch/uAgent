import type { RefObject } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Redo2, Undo2 } from "lucide-preact";
import { Button, IconButton } from "../../shared/ui.tsx";
import { Input } from "../../shared/form-controls.tsx";
import { Tabs } from "../../shared/tabs.tsx";
import { plural } from "../../shared/quantities.ts";
import type { Board } from "./boards.ts";
import { useCurrentInView } from "./legend.tsx";
import type { Cell } from "./sheet-edit.ts";

// A sheet's chrome around its grid, which the page draws and edits itself.

// Enter writes into the cell; Escape puts back what the cell holds.
export function FormulaBar({
  cell,
  locked,
  write,
  field,
}: {
  cell?: Cell;
  locked: boolean;
  write: (value: string) => void;
  field: RefObject<HTMLInputElement | null>;
}) {
  const [typed, setTyped] = useState<string>();
  // Another cell, or this one holding something else: what was typed goes.
  useEffect(() => setTyped(undefined), [cell?.ref, cell?.raw]);
  return (
    <div class="sheet-formula small" role="group" aria-label="Formula bar">
      <output>{cell?.ref}</output>
      <Input
        aria-label="Cell contents"
        inputRef={field}
        disabled={!cell}
        readOnly={locked}
        value={typed ?? cell?.raw ?? ""}
        onInput={(event) => setTyped(event.currentTarget.value)}
        onKeyDown={(event) => {
          if (event.key === "Enter") write(event.currentTarget.value);
          else if (event.key === "Escape") setTyped(undefined);
          else return;
          event.preventDefault();
        }}
      />
    </div>
  );
}

const number = (value: number) =>
  value.toLocaleString(undefined, { maximumFractionDigits: 2 });

// A sheet's legend, as tabs. On a phone no key reaches the grid: Edit cell
// brings up the keyboard on the formula bar.
export function SheetBar({
  boards,
  index,
  to,
  cell,
  hidden,
  undo,
  redo,
  locked,
  act,
  edit,
  comment,
}: {
  boards: Board[];
  index: number;
  to: (index: number) => void;
  cell?: Cell;
  hidden: number;
  undo: boolean;
  redo: boolean;
  // An Excel or CSV file: read, and saved as a sheet to change it.
  locked: boolean;
  act: (command: "undo" | "redo" | "chart" | "sheet") => void;
  edit: () => void;
  comment?: () => void;
}) {
  const list = useRef<HTMLDivElement>(null);
  useCurrentInView(list, index);
  const { summary } = cell ?? {};
  const note = [
    locked && "Read-only",
    hidden && `${plural(hidden, "row")} hidden`,
    summary &&
      `Sum ${number(summary.sum)} · Average ${number(summary.average)} · Count ${summary.count}`,
  ].filter(Boolean);
  return (
    <div class="sheet-bar">
      <Tabs
        label="Tabs"
        size="compact"
        class="sheet-tabs"
        listRef={list}
        value={index}
        to={to}
        tabs={boards.map((board, at) => ({ key: at, label: board.title }))}
      />
      {note.length > 0 && (
        <span class="sheet-note small muted truncate" role="status">
          {note.join(" · ")}
        </span>
      )}
      {locked ? (
        <Button variant="secondary" onClick={() => act("sheet")}>
          Save as sheet
        </Button>
      ) : (
        <>
          <IconButton label="Undo" disabled={!undo} onClick={() => act("undo")}>
            <Undo2 />
          </IconButton>
          <IconButton label="Redo" disabled={!redo} onClick={() => act("redo")}>
            <Redo2 />
          </IconButton>
          <Button variant="secondary" onClick={() => act("chart")}>
            Chart
          </Button>
          <Button variant="secondary" class="sheet-edit-cell" onClick={edit}>
            Edit cell
          </Button>
        </>
      )}
      {comment && (
        <Button variant="secondary" disabled={!cell} onClick={comment}>
          Comment
        </Button>
      )}
    </div>
  );
}
