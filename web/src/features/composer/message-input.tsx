import type { JSX, RefObject } from "preact";
import { Textarea } from "../../shared/form-controls.tsx";

// Without a hardware pointer the on-screen keyboard's return key writes a
// new line, as in native messaging apps; the Send button sends, and so does
// Ctrl/Cmd+Enter from a hardware keyboard on a tablet.
const softKeyboard = matchMedia("(pointer: coarse) and (hover: none)");

// Main and child composers share growth, IME handling and submit semantics.
export default function MessageInput({
  inputRef,
  submit,
  onKeyDown,
  resizeKey,
  ...props
}: JSX.TextareaHTMLAttributes<HTMLTextAreaElement> & {
  inputRef?: RefObject<HTMLTextAreaElement>;
  resizeKey?: number;
  submit: (event: Event) => void;
}) {
  return (
    <Textarea
      class="message-input"
      grow
      resizeKey={resizeKey}
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
