import type { ComponentChildren, RefObject } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import {
  Baseline,
  Bold,
  Check,
  ChevronDown,
  Hash,
  Italic,
  PaintBucket,
  Plus,
  Redo2,
  TextAlignCenter,
  TextAlignEnd,
  TextAlignStart,
  TextWrap,
  Undo2,
} from "lucide-preact";
import {
  Actions,
  Button,
  ConfirmModal,
  Field,
  IconButton,
  Modal,
  Notice,
} from "../../shared/ui.tsx";
import { Input } from "../../shared/form-controls.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import { SheetButton } from "../../shared/sheet.tsx";
import { Tabs } from "../../shared/tabs.tsx";
import { capitalize } from "../../shared/display.ts";
import { plural } from "../../shared/quantities.ts";
import type { Board } from "./boards.ts";
import { useCurrentInView } from "./legend.tsx";
import type { Cell } from "./sheet-edit.ts";

// A sheet's chrome around its grid, which the page draws and edits itself.

type Act = (command: string, value?: string) => void;
type Options = [string, string][];

// The looks a cell may take (skills/artifact/references/sheet.md): the
// theme's fills and text colours by name, a few number patterns.
const COLOURS: Options = [
  ["Accent", "accent"],
  ["Good", "good"],
  ["Warning", "warn"],
  ["Bad", "bad"],
  ["Muted", "muted"],
];
const ALIGNS = {
  left: TextAlignStart,
  center: TextAlignCenter,
  right: TextAlignEnd,
};
const FORMATS: Options = [
  ["General", "general"],
  ["Number", "#,##0.00"],
  ["Currency", "$#,##0.00"],
  ["Percent", "0%"],
  ["Date", "yyyy-mm-dd"],
];

// One of a few, from a menu under an icon: the one the cell has is ticked.
// A colour has its swatch before its name.
function Choice({
  label,
  icon,
  options,
  value,
  pick,
  swatch,
}: {
  label: string;
  icon: ComponentChildren;
  options: Options;
  value: string;
  pick: (value: string) => void;
  swatch?: "fill" | "ink";
}) {
  return (
    <Menu
      label={label}
      trigger={
        <>
          {icon}
          <span class="sr-only">{label}</span>
          <ChevronDown />
        </>
      }
    >
      {options.map(([name, option]) => (
        <MenuItem
          key={option}
          role="menuitemradio"
          aria-checked={option === value}
          onClick={() => pick(option)}
        >
          <span data-look={swatch && option ? `${swatch}-${option}` : null}>
            {name}
          </span>
          {option === value && <Check />}
        </MenuItem>
      ))}
    </Menu>
  );
}

// How the selection's cells look, set as the active cell shows it.
function Looks({ cell, act }: { cell?: Cell; act: Act }) {
  const { bold, italic, wrap, align, fill, ink, format } = cell?.marks ?? {};
  const Aligned = ALIGNS[align as keyof typeof ALIGNS] ?? TextAlignStart;
  return (
    <>
      <IconButton
        label="Bold"
        aria-pressed={!!bold}
        onClick={() => act("bold")}
      >
        <Bold />
      </IconButton>
      <IconButton
        label="Italic"
        aria-pressed={!!italic}
        onClick={() => act("italic")}
      >
        <Italic />
      </IconButton>
      <Choice
        label="Text colour"
        icon={<Baseline />}
        options={[["Default", ""], ...COLOURS.slice(1)]}
        swatch="ink"
        value={ink ?? ""}
        pick={(value) => act("ink", value)}
      />
      <Choice
        label="Fill"
        icon={<PaintBucket />}
        options={[["None", ""], ...COLOURS]}
        swatch="fill"
        value={fill ?? ""}
        pick={(value) => act("fill", value)}
      />
      <Choice
        label="Align"
        icon={<Aligned />}
        options={Object.keys(ALIGNS).map((name) => [capitalize(name), name])}
        value={align ?? ""}
        pick={(value) => act("align", value)}
      />
      <Choice
        label="Number format"
        icon={<Hash />}
        options={FORMATS}
        value={(format ?? "").toLowerCase()}
        pick={(value) => act("format", value)}
      />
      <IconButton
        label="Wrap text"
        aria-pressed={!!wrap}
        onClick={() => act("wrap")}
      >
        <TextWrap />
      </IconButton>
    </>
  );
}

// Enter writes into the cell; Escape puts back what the cell holds. Beside
// it the cell's looks, which a narrow pane folds into one sheet.
export function FormulaBar({
  cell,
  typing,
  locked,
  write,
  act,
  field,
}: {
  cell?: Cell;
  // What is being typed in the cell itself.
  typing?: string;
  locked: boolean;
  write: (value: string) => void;
  act: Act;
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
        value={typed ?? typing ?? cell?.raw ?? ""}
        onInput={(event) => setTyped(event.currentTarget.value)}
        onKeyDown={(event) => {
          if (event.key === "Enter") write(event.currentTarget.value);
          else if (event.key === "Escape") setTyped(undefined);
          else return;
          event.preventDefault();
        }}
      />
      {!locked && (
        <>
          <div class="sheet-looks">
            <Looks cell={cell} act={act} />
          </div>
          <SheetButton
            className="sheet-looks-folded"
            label="Format"
            trigger={
              <>
                Format
                <ChevronDown />
              </>
            }
          >
            <div class="sheet-looks-sheet">
              <Looks cell={cell} act={act} />
            </div>
          </SheetButton>
        </>
      )}
    </div>
  );
}

const number = (value: number) =>
  value.toLocaleString(undefined, { maximumFractionDigits: 2 });

// What is done to the tab shown, from its menu beside the tabs: a tablist
// holds only its tabs. A new name is typed; a tab is taken out after a
// question that says how many cells of other tabs read it.
function TabActions({
  boards,
  index,
  readers,
  act,
}: {
  boards: Board[];
  index: number;
  readers: number;
  act: Act;
}) {
  const [asking, setAsking] = useState<"" | "name" | "delete">("");
  const [name, setName] = useState("");
  const close = () => setAsking("");
  const { title } = boards[index];
  return (
    <>
      <Menu label="Tab actions">
        <MenuItem
          onClick={() => {
            setName(title);
            setAsking("name");
          }}
        >
          Rename…
        </MenuItem>
        <MenuItem onClick={() => act("tab-copy")}>Duplicate</MenuItem>
        <MenuItem disabled={!index} onClick={() => act("tab-left")}>
          Move left
        </MenuItem>
        <MenuItem
          disabled={index === boards.length - 1}
          onClick={() => act("tab-right")}
        >
          Move right
        </MenuItem>
        <MenuItem
          disabled={boards.length < 2}
          onClick={() => setAsking("delete")}
        >
          Delete…
        </MenuItem>
      </Menu>
      {asking === "name" && (
        <Modal title="Rename tab" close={close} size="s">
          <form
            onSubmit={(event) => {
              event.preventDefault();
              close();
              act("tab-name", name);
            }}
          >
            <Field label="Name">
              <Input
                value={name}
                maxLength={31}
                autoFocus
                onInput={(event) => setName(event.currentTarget.value)}
              />
            </Field>
            <Actions>
              <Button onClick={close}>Cancel</Button>
              <Button type="submit" variant="primary">
                Rename
              </Button>
            </Actions>
          </form>
        </Modal>
      )}
      {asking === "delete" && (
        <ConfirmModal
          title="Delete tab"
          close={close}
          confirm={() => {
            close();
            act("tab-delete");
          }}
        >
          {`${title} is deleted from the sheet.${
            readers
              ? ` ${plural(readers, "cell")} of other tabs ${readers === 1 ? "reads" : "read"} it and will show an error.`
              : ""
          } Undo brings it back.`}
        </ConfirmModal>
      )}
    </>
  );
}

// A sheet's legend, as tabs. On a phone no key reaches the grid: Edit cell
// brings up the keyboard on the formula bar.
export function SheetBar({
  boards,
  index,
  to,
  cell,
  hidden,
  readers,
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
  // How many cells of other tabs read the tab shown.
  readers: number;
  undo: boolean;
  redo: boolean;
  // An Excel or CSV file: read, and saved as a sheet to change it.
  locked: boolean;
  act: Act;
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
      <div class="sheet-tabs" ref={list}>
        <Tabs
          label="Tabs"
          size="compact"
          value={index}
          to={to}
          tabs={boards.map((board, at) => ({ key: at, label: board.title }))}
        />
        {!locked && (
          <>
            <IconButton label="Add tab" onClick={() => act("tab-add")}>
              <Plus />
            </IconButton>
            <TabActions
              boards={boards}
              index={index}
              readers={readers}
              act={act}
            />
          </>
        )}
      </div>
      {note.length > 0 && (
        <small class="sheet-note muted">
          <Notice tone="info">{note.join(" · ")}</Notice>
        </small>
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
