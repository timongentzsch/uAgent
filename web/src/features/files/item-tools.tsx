import type { ComponentChildren } from "preact";
import {
  Check,
  ChevronDown,
  Copy,
  Frame,
  MessageSquare,
  MousePointer2,
  MoveUpRight,
  RotateCcw,
  Square,
  SquareDashed,
  StickyNote,
  Table,
  Trash2,
  Type,
} from "lucide-preact";
import { InsertBar, PickBar, type Tool } from "../../shared/float-bars.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import type { PageMessage, Rect } from "../../shared/page-frame.tsx";
import { IconButton } from "../../shared/ui.tsx";
import { maxItemEntries } from "../../shared/limits.ts";
import { boardEdit, FILLS, SHAPES, TEXTS } from "./board-edit.ts";
import { SLIDE_MADE, slideEdit } from "./doc-edit.ts";

// The tools over a stage whose items are changed by hand (web/sheet/items.js):
// what the next press puts on it, and what can be done to what is picked
// there. A whiteboard and a slide each have theirs.

type Act = (act: string, value?: string) => void;
// What is picked, as its page said it: the page is untrusted.
export type Chosen = Extract<PageMessage, { type: "items" }>;
type Item = Chosen["items"][number];
export interface Items {
  tools: Tool[];
  // What a tool puts in, which the page is armed with.
  made?: typeof SLIDE_MADE;
  // The change the page asks, made in its file's text; null where the file
  // does not take it.
  edit(source: string, change: never): string | null;
  // The controls for one item picked, before Duplicate, Delete and Ask.
  one: (item: Item, act: Act) => ComponentChildren;
}

const named = (name: string) => name[0].toUpperCase() + name.slice(1);
const SIZES: Record<string, string> = {
  h1: "Title",
  h2: "Large",
  p: "Body",
  small: "Small",
};
const SLIDE_SHAPES = Object.entries(SLIDE_MADE).flatMap(
  ([kind, { label, height }]): [string, string][] =>
    height ? [[kind, label]] : [],
);

// A menu of the values `name` takes, the one it has checked.
const choice = (
  act: Act,
  label: string,
  name: string,
  value: string,
  options: [string, string][],
  swatch?: boolean,
) => (
  <Menu
    label={label}
    trigger={
      <>
        {/* A colour is its swatch. */}
        {swatch && <span data-look={`board-${value}`} />}
        <span class={swatch ? "sr-only" : undefined}>
          {options.find(([option]) => option === value)?.[1] ?? label}
        </span>
        <ChevronDown />
      </>
    }
  >
    {options.map(([option, text]) => (
      <MenuItem
        key={option}
        role="menuitemradio"
        aria-checked={option === value}
        onClick={() => act(name, option)}
      >
        <span data-look={swatch ? `board-${option}` : undefined}>{text}</span>
        {option === value && <Check />}
      </MenuItem>
    ))}
  </Menu>
);
const order = (act: Act) => (
  <Menu label="Order">
    <MenuItem onClick={() => act("front")}>Bring to front</MenuItem>
    <MenuItem onClick={() => act("back")}>Send to back</MenuItem>
  </Menu>
);

export const BOARD: Items = {
  tools: [
    { name: "", label: "Select", icon: <MousePointer2 /> },
    { name: "note", label: "Note", icon: <StickyNote /> },
    { name: "text", label: "Text", icon: <Type /> },
    {
      name: "shape",
      label: "Shape",
      icon: <Square />,
      kinds: SHAPES.map((kind) => [kind, named(kind)]),
    },
    { name: "section", label: "Section", icon: <Frame /> },
    { name: "arrow", label: "Arrow", icon: <MoveUpRight /> },
  ],
  edit: boardEdit,
  one: (one, act) => (
    <>
      {one.shape &&
        choice(
          act,
          "Shape",
          "shape",
          one.shape,
          SHAPES.map((kind) => [kind, named(kind)]),
        )}
      {(one.shape || one.note) &&
        choice(
          act,
          "Colour",
          "fill",
          one.fill ?? (one.note ? "yellow" : "grey"),
          FILLS.map((fill) => [fill, named(fill)]),
          true,
        )}
      {!one.note &&
        TEXTS.includes(one.tag) &&
        choice(act, "Text size", "tag", one.tag, Object.entries(SIZES))}
      {order(act)}
    </>
  ),
};

export const SLIDE: Items = {
  tools: [
    { name: "", label: "Select", icon: <MousePointer2 /> },
    { name: "text", label: SLIDE_MADE.text.label, icon: <Type /> },
    { name: "shape", label: "Shape", icon: <Square />, kinds: SLIDE_SHAPES },
    { name: "table", label: SLIDE_MADE.table.label, icon: <Table /> },
  ],
  made: SLIDE_MADE,
  edit: slideEdit,
  // Its container picked; its own place and size again; and of what was
  // put in by hand, the order.
  one: (one, act) => (
    <>
      {one.container && (
        <IconButton label="Select container" onClick={() => act("container")}>
          <SquareDashed />
        </IconButton>
      )}
      {one.moved && (
        <IconButton label="Put back" onClick={() => act("reset")}>
          <RotateCcw />
        </IconButton>
      )}
      {one.placed && order(act)}
    </>
  ),
};

export function chosenOf(message: Chosen): Chosen | undefined {
  const [x, y, width, height] = ["x", "y", "width", "height"].map((side) =>
    Number(message.rect?.[side as keyof Rect]),
  );
  if (!Array.isArray(message.items)) return undefined;
  const shapes = [...SHAPES, ...SLIDE_SHAPES.map(([kind]) => kind)];
  return {
    type: "items",
    items: message.items
      .slice(0, maxItemEntries)
      .filter((item) => item && typeof item === "object")
      .map((item) => ({
        selector: String(item.selector).slice(0, 2000),
        tag: String(item.tag),
        text: String(item.text).slice(0, 60),
        shape: shapes.find((kind) => kind === item.shape),
        fill: FILLS.find((fill) => fill === item.fill),
        note: !!item.note,
        container: !!item.container,
        moved: !!item.moved,
        placed: !!item.placed,
      })),
    arrow: !!message.arrow,
    // None while nothing is picked.
    rect: [x, y, width, height].every(Number.isFinite)
      ? { x, y, width, height }
      : undefined,
  };
}

export function ItemTools({
  items,
  chosen,
  tool,
  arm,
  act,
  ask,
  at,
}: {
  items: Items;
  // What the page says is picked: it takes presses.
  chosen: Chosen;
  tool: string;
  arm: (tool: string) => void;
  // A change asked of what is picked.
  act: Act;
  // A comment on the one item picked.
  ask?: () => void;
  // Where what is picked is, in the window.
  at: () => Rect | undefined;
}) {
  const [one, two, three] = chosen.items;
  const remove = (
    <IconButton label="Delete" onClick={() => act("delete")}>
      <Trash2 />
    </IconButton>
  );
  const duplicate = (
    <IconButton label="Duplicate" onClick={() => act("duplicate")}>
      <Copy />
    </IconButton>
  );
  return (
    <>
      <InsertBar tools={items.tools} armed={tool} arm={arm} />
      {(one || chosen.arrow) && (
        <PickBar label="Picked" at={at}>
          {chosen.arrow ? (
            remove
          ) : two ? (
            <>
              {choice(act, "Align", "align", "", [
                ["left", "Left"],
                ["center", "Centre"],
                ["right", "Right"],
                ["top", "Top"],
                ["middle", "Middle"],
                ["bottom", "Bottom"],
              ])}
              {three &&
                choice(act, "Distribute", "distribute", "", [
                  ["across", "Across"],
                  ["down", "Down"],
                ])}
              {duplicate}
              {remove}
            </>
          ) : (
            <>
              {items.one(one, act)}
              {duplicate}
              {remove}
              {ask && (
                <IconButton label="Ask about it" onClick={ask}>
                  <MessageSquare />
                </IconButton>
              )}
            </>
          )}
        </PickBar>
      )}
    </>
  );
}
