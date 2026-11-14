import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import {
  Bold,
  Check,
  ChevronDown,
  Italic,
  Link,
  Pilcrow,
  Plus,
} from "lucide-preact";
import { Menu, MenuItem } from "../../shared/menu.tsx";
import type { PageMessage } from "../../shared/page-frame.tsx";
import { Button, IconButton, Input, InputRow } from "../../shared/ui.tsx";
import { INSERTS, KINDS, kindOf, LINK } from "./doc-edit.ts";

// What is selected in the block being edited, as its page says it
// (design-helper.js): the block's kind where it can become another, whether
// anything is selected, its marks (null where the block's own style is the
// mark) and the link it touches.
export interface Caret {
  kind: string;
  selected: boolean;
  bold: boolean | null;
  italic: boolean | null;
  href: string;
}
// The page is untrusted: only what the tools show is taken. None while no
// block is being edited.
export const caretOf = (
  said: Extract<PageMessage, { type: "caret" }>,
): Caret | undefined =>
  typeof said.tag === "string"
    ? {
        kind: kindOf(said.tag, String(said.parent)),
        selected: said.selected === true,
        bold: said.bold === null ? null : said.bold === true,
        italic: said.italic === null ? null : said.italic === true,
        href: String(said.href ?? "").slice(0, 2000),
      }
    : undefined;

export type Format =
  | { mark: "bold" | "italic" }
  // Its address; empty takes the link away.
  | { href: string }
  | { as: string }
  | { insert: string };

// Edit's tools, in a row of their own while Edit is on, each disabled where
// it does not apply. A press leaves the focus, and so the selection, in the
// page.
export function FormatTools({
  caret,
  format,
  inserts,
  linking,
  link,
}: {
  caret?: Caret;
  format: (format: Format) => void;
  // Whether blocks can be put in: a document's text flows on, a slide's
  // does not.
  inserts: boolean;
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
    <fieldset
      class="page-asks"
      disabled={!caret}
      onMouseDown={(event) => event.preventDefault()}
    >
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
      {inserts && (
        <Menu
          label="Insert after this block"
          trigger={
            <>
              <Plus />
              Insert
              <ChevronDown />
            </>
          }
        >
          {Object.entries(INSERTS).map(([kind, name]) => (
            <MenuItem key={kind} onClick={() => format({ insert: kind })}>
              {name}
            </MenuItem>
          ))}
        </Menu>
      )}
    </fieldset>
  );
}

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
