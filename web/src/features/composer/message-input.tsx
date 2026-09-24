import type { JSX, RefObject } from "preact";
import { Textarea } from "../../shared/form-controls.tsx";

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
      {...props}
      inputRef={inputRef}
      onKeyDown={(event) => {
        onKeyDown?.(event);
        if (
          event.defaultPrevented ||
          event.key !== "Enter" ||
          event.shiftKey ||
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
