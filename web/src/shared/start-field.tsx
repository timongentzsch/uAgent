import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import { ArrowUp } from "lucide-preact";
import { useHostContext } from "../state/host-context.ts";
import { IconButton } from "./ui.tsx";
import MessageInput from "./message-input.tsx";
import { useAction } from "./use-action.ts";
import "./start-field.css";

// The box a conversation starts from: what to ask, then a row of what it
// starts with (`children`: a place, a model) and the button that starts it.
export function StartField({
  id,
  label,
  placeholder,
  disabled,
  autoFocus,
  action = "Start",
  onKeyDown,
  start,
  children,
}: {
  id?: string;
  label: string;
  placeholder: string;
  disabled?: boolean;
  autoFocus?: boolean;
  // What its button does, for whoever cannot see the arrow.
  action?: string;
  onKeyDown?: (event: KeyboardEvent) => void;
  start: (text: string) => Promise<unknown> | undefined;
  children?: (text: string) => ComponentChildren;
}) {
  const { report } = useHostContext();
  const [text, setText] = useState("");
  const { run } = useAction({ report });
  const submit = (event: Event) => {
    event.preventDefault();
    void run(async () => start(text.trim())).then(
      (done) => done && setText(""),
    );
  };
  return (
    <form class="start-field message-box" onSubmit={submit}>
      <MessageInput
        id={id}
        rows={1}
        aria-label={label}
        placeholder={placeholder}
        value={text}
        disabled={disabled}
        autoFocus={autoFocus}
        submit={submit}
        onKeyDown={onKeyDown}
        onInput={(event) => setText(event.currentTarget.value)}
      />
      <div class="start-tools message-actions">
        {children?.(text.trim())}
        <IconButton
          label={action}
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
