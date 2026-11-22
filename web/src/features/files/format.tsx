import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import {
  Bold,
  Check,
  ChevronDown,
  Code,
  Heading2,
  Heading3,
  Italic,
  Link,
  List,
  ListChecks,
  ListOrdered,
  Minus,
  Pilcrow,
  Table,
  TextQuote,
} from "lucide-preact";
import type { Tool } from "../../shared/float-bars.tsx";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import {
  type PageMessage,
  type Rect,
  rectOf,
} from "../../shared/page-frame.tsx";
import { Button, IconButton, Input, InputRow } from "../../shared/ui.tsx";
import { INSERTS, KINDS, kindOf, LINK } from "./doc-edit.ts";

// What is selected in the block being edited, as its page says it
// (design-helper.js): the block's kind where it can become another, whether
// anything is selected, its marks (null where the block's own style is the
// mark), the link it touches; where what is selected is in its page, and
// the block as a comment names it.
export interface Caret {
  kind: string;
  selected: boolean;
  bold: boolean | null;
  italic: boolean | null;
  href: string;
  rect: Rect;
  tag: string;
  selector: string;
  text: string;
}
// The page is untrusted: only what the tools show is taken. None while no
// block is being edited.
export const caretOf = (
  said: Extract<PageMessage, { type: "caret" }>,
): Caret | undefined => {
  const rect = rectOf(said.rect);
  return typeof said.tag === "string" && rect
    ? {
        kind: kindOf(said.tag, String(said.parent)),
        selected: said.selected === true,
        bold: said.bold === null ? null : said.bold === true,
        italic: said.italic === null ? null : said.italic === true,
        href: String(said.href ?? "").slice(0, 2000),
        rect,
        tag: said.tag.slice(0, 40),
        selector: String(said.selector).slice(0, 2000),
        text: String(said.text ?? "").slice(0, 60),
      }
    : undefined;
};

export type Format =
  | { mark: "bold" | "italic" }
  // Its address; empty takes the link away.
  | { href: string }
  | { as: string }
  | { insert: string };

// What is done to the text being edited, over its block while Edit is on,
// each disabled where it does not apply.
export function TextTools({
  caret,
  format,
  linking,
  link,
}: {
  caret?: Caret;
  format: (format: Format) => void;
  // Whether the link's field is open, and what opens or closes it.
  linking: boolean;
  link: () => void;
}) {
  const mark = (name: "bold" | "italic", icon: ComponentChildren) => (
    <IconButton
      label={name === "bold" ? "Bold" : "Italic"}
      aria-pressed={!!caret?.[name]}
      disabled={!caret?.selected || caret[name] === null}
      onClick={() => format({ mark: name })}
    >
      {icon}
    </IconButton>
  );
  return (
    <>
      <Menu
        label="Text style"
        trigger={
          <>
            <Pilcrow />
            Style
            <ChevronDown />
          </>
        }
      >
        {Object.entries(KINDS).map(([kind, name]) => (
          <MenuItem
            key={kind}
            role="menuitemradio"
            aria-checked={kind === caret?.kind}
            disabled={!caret?.kind}
            onClick={() => kind !== caret?.kind && format({ as: kind })}
          >
            {name}
            {kind === caret?.kind && <Check />}
          </MenuItem>
        ))}
      </Menu>
      {mark("bold", <Bold />)}
      {mark("italic", <Italic />)}
      <IconButton
        label="Link"
        aria-pressed={linking}
        disabled={!caret?.selected && !caret?.href}
        onClick={link}
      >
        <Link />
      </IconButton>
    </>
  );
}

// What a document's insert bar puts in after the block being edited.
const INSERT_ICONS: Record<string, ComponentChildren> = {
  p: <Pilcrow />,
  h2: <Heading2 />,
  h3: <Heading3 />,
  ul: <List />,
  ol: <ListOrdered />,
  checklist: <ListChecks />,
  blockquote: <TextQuote />,
  pre: <Code />,
  table: <Table />,
  hr: <Minus />,
};
export const DOC_INSERTS: Tool[] = Object.entries(INSERTS).map(
  ([name, label]) => ({ name, label, icon: INSERT_ICONS[name] }),
);

// The address of the link on what is selected, typed under the tools; Esc
// or the Link tool shuts it.
export function LinkField({
  href,
  apply,
  close,
}: {
  href: string;
  apply: (href: string) => void;
  close: () => void;
}) {
  const [address, setAddress] = useState(href);
  const field = useRef<HTMLInputElement>(null);
  useEffect(() => field.current?.focus(), []);
  const valid = LINK.test(address.trim());
  return (
    <InputRow
      as="form"
      class="find-bar"
      aria-label="Link"
      onSubmit={(event) => {
        event.preventDefault();
        if (valid) apply(address.trim());
      }}
      onKeyDown={(event) => {
        if (event.key !== "Escape") return;
        event.preventDefault();
        event.stopPropagation();
        close();
      }}
    >
      <Input
        inputMode="url"
        aria-label="Link address"
        placeholder="https://… or #place"
        inputRef={field}
        value={address}
        onInput={(event) => setAddress(event.currentTarget.value)}
      />
      <Button type="submit" variant="primary" disabled={!valid}>
        Apply
      </Button>
      <Button disabled={!href} onClick={() => apply("")}>
        Remove
      </Button>
    </InputRow>
  );
}
