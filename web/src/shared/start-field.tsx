import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import { ArrowUp } from "lucide-preact";
import { useHostContext } from "../state/host-context.ts";
import { IconButton } from "./ui.tsx";
import MessageInput from "./message-input.tsx";
import "./start-field.css";

// The box a conversation starts from: what to ask, then a row of what it
// starts with (`children`: a place, a model) and the button that starts it.
export function StartField({
  id,
  label,
  placeholder,
  disabled,
  autoFocus,
  start,
  children,
}: {
  id?: string;
  label: string;
  placeholder: string;
  disabled?: boolean;
  autoFocus?: boolean;
  start: (text: string) => Promise<unknown> | undefined;
  children?: (text: string) => ComponentChildren;
}) {
  const { report } = useHostContext();
  const [text, setText] = useState("");
  const submit = (event: Event) => {
    event.preventDefault();
    Promise.resolve(start(text.trim())).then(() => setText(""), report);
  };
  return (
    <form class="start-field" onSubmit={submit}>
      <MessageInput
        id={id}
        rows={1}
        aria-label={label}
        placeholder={placeholder}
        value={text}
        disabled={disabled}
        autoFocus={autoFocus}
        submit={submit}
        onInput={(event) => setText(event.currentTarget.value)}
      />
      <div class="start-tools">
        {children?.(text.trim())}
        <IconButton
          label="Start"
          variant="primary"
          type="submit"
          disabled={disabled}
        >
          <ArrowUp />
        </IconButton>
      </div>
    </form>
  );
}
