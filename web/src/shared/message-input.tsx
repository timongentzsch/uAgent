import type { ComponentChildren, RefObject } from "preact";
import { X } from "lucide-preact";
import { Textarea, type TextareaProps } from "./form-controls.tsx";
import { IconButton } from "./ui.tsx";
import "./message-input.css";

// Without a hardware pointer the on-screen keyboard's return key writes a
// new line, as in native messaging apps; the Send button sends, and so does
// Ctrl/Cmd+Enter from a hardware keyboard on a tablet.
const softKeyboard = matchMedia("(pointer: coarse) and (hover: none)");

export default function MessageInput({
  inputRef,
  submit,
  onKeyDown,
  ...props
}: TextareaProps & {
  inputRef?: RefObject<HTMLTextAreaElement | null>;
  submit: (event: Event) => void;
}) {
  return (
    <Textarea
      class="message-input"
      grow
      enterkeyhint={softKeyboard.matches ? "enter" : "send"}
      {...props}
      inputRef={inputRef}
      onKeyDown={(event) => {
        onKeyDown?.(event);
        if (
          event.defaultPrevented ||
          event.key !== "Enter" ||
          event.shiftKey ||
          (softKeyboard.matches && !event.ctrlKey && !event.metaKey) ||
          event.isComposing ||
          event.keyCode === 229
        )
          return;
        event.preventDefault();
        if (!event.repeat) submit(event);
      }}
    />
  );
}

// A quote's first line of words, without its markdown.
const firstLine = (quote: string) =>
  quote
    .split("\n")
    .filter((line) => !line.startsWith("```"))
    .map((line) => line.replace(/^> ?|`/g, "").trim())
    .find(Boolean) ?? "";

// What a message quotes, one line over its draft in the message box;
// `children` say what it is quoted for.
export function QuoteBar({
  quote,
  remove,
  label = "Remove the quote",
  children,
}: {
  quote: string;
  remove: () => void;
  label?: string;
  children?: ComponentChildren;
}) {
  return (
    <div class="quote-bar">
      {children}
      <span class="muted truncate">{firstLine(quote)}</span>
      <IconButton label={label} onClick={remove}>
        <X />
      </IconButton>
    </div>
  );
}
