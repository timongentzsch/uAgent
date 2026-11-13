import type { ComponentChildren, TargetedSubmitEvent } from "preact";
import { useState } from "preact/hooks";
import { useHostContext } from "../state/host-context.ts";
import { Button, Input } from "./ui.tsx";
import "./start-field.css";

// A line of text that starts a conversation; `children` adds controls that
// act on what is typed.
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
  const submit = (event: TargetedSubmitEvent<HTMLFormElement>) => {
    event.preventDefault();
    Promise.resolve(start(text.trim())).then(() => setText(""), report);
  };
  return (
    <form class="start-field" onSubmit={submit}>
      <Input
        id={id}
        aria-label={label}
        placeholder={placeholder}
        value={text}
        disabled={disabled}
        autoFocus={autoFocus}
        onInput={(event) => setText(event.currentTarget.value)}
      />
      <Button variant="primary" type="submit" disabled={disabled}>
        Start
      </Button>
      {children?.(text.trim())}
    </form>
  );
}
