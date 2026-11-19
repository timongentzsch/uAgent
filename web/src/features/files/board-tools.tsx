import {
  Check,
  ChevronDown,
  Copy,
  Frame,
  MessageSquare,
  MousePointer2,
  MoveUpRight,
  Square,
  StickyNote,
  Trash2,
  Type,
} from "lucide-preact";
import { InsertBar, PickBar, type Tool } from "../../shared/float-bars.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import type { PageMessage, Rect } from "../../shared/page-frame.tsx";
import { IconButton } from "../../shared/ui.tsx";
import { FILLS, SHAPES, TEXTS } from "./board-edit.ts";

// A whiteboard's tools over its stage: what the next press puts on it, and
// what can be done to what is picked there (web/sheet/whiteboard.js).

const named = (name: string) => name[0].toUpperCase() + name.slice(1);
const SIZES: Record<string, string> = {
  h1: "Title",
  h2: "Large",
  p: "Body",
  small: "Small",
};
const TOOLS: Tool[] = [
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
];

// What is picked on a board, as its page said it: the page is untrusted.
export type Chosen = Extract<PageMessage, { type: "items" }>;
export const chosenOf = (message: Chosen): Chosen | undefined =>
  Array.isArray(message.items) && message.rect
    ? {
        type: "items",
        items: message.items.slice(0, 1000).map((item) => ({
          id: String(item.id),
          tag: String(item.tag),
          text: String(item.text).slice(0, 60),
          shape: SHAPES.find((kind) => kind === item.shape),
          fill: FILLS.find((fill) => fill === item.fill),
          note: !!item.note,
        })),
        arrow: !!message.arrow,
        rect: message.rect,
      }
    : undefined;

export function BoardTools({
  chosen,
  tool,
  arm,
  act,
  ask,
  at,
}: {
  chosen?: Chosen;
  tool: string;
  arm: (tool: string) => void;
  // A change asked of what is picked.
  act: (act: string, value?: string) => void;
  // A comment on the one item picked.
  ask?: () => void;
  // Where what is picked is, in the window.
  at: () => Rect | undefined;
}) {
  const [one, two, three] = chosen?.items ?? [];
  // A menu of the values `name` takes, the one it has checked.
  const choice = (
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
  const text = one && !one.note && TEXTS.includes(one.tag);
  return (
    <>
      <InsertBar tools={TOOLS} armed={tool} arm={arm} />
      {(one || chosen?.arrow) && (
        <PickBar label="Picked" at={at}>
          {chosen?.arrow ? (
            remove
          ) : two ? (
            <>
              {choice("Align", "align", "", [
                ["left", "Left"],
                ["center", "Centre"],
                ["right", "Right"],
                ["top", "Top"],
                ["middle", "Middle"],
                ["bottom", "Bottom"],
              ])}
              {three &&
                choice("Distribute", "distribute", "", [
                  ["across", "Across"],
                  ["down", "Down"],
                ])}
              {duplicate}
              {remove}
            </>
          ) : (
            <>
              {one.shape &&
                choice(
                  "Shape",
                  "shape",
                  one.shape,
                  SHAPES.map((kind) => [kind, named(kind)]),
                )}
              {(one.shape || one.note) &&
                choice(
                  "Colour",
                  "fill",
                  one.fill ?? (one.note ? "yellow" : "grey"),
                  FILLS.map((fill) => [fill, named(fill)]),
                  true,
                )}
              {text &&
                choice("Text size", "tag", one.tag, Object.entries(SIZES))}
              <Menu label="Order">
                <MenuItem onClick={() => act("front")}>Bring to front</MenuItem>
                <MenuItem onClick={() => act("back")}>Send to back</MenuItem>
              </Menu>
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
