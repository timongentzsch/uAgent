import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";
import { ArrowUp } from "lucide-preact";
import { useHostContext } from "../state/host-context.ts";
import { IconButton } from "./ui.tsx";
import MessageInput from "./message-input.tsx";
import { useAction } from "./use-action.ts";
import { AttachButton, FileChip, pastedFiles } from "./attachments.tsx";
import { bytes } from "./quantities.ts";
import { uploadLimit } from "./limits.ts";
import "./attachments.css";
import "./start-field.css";

// The box a conversation starts from: what to ask, then a row of what it
// starts with (`children`: a place, a model) and the button that starts it.
// With `attach`, files go with the first message, as from the composer.
export function StartField({
  id,
  label,
  placeholder,
  disabled,
  autoFocus,
  action = "Start",
  attach,
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
  attach?: boolean;
  onKeyDown?: (event: KeyboardEvent) => void;
  start: (text: string, files: File[]) => Promise<unknown> | undefined;
  children?: (text: string) => ComponentChildren;
}) {
  const { report } = useHostContext();
  const [text, setText] = useState("");
  const [files, setFiles] = useState<File[]>([]);
  const { run } = useAction({ report });
  const submit = (event: Event) => {
    event.preventDefault();
    void run(async () => start(text.trim(), files)).then((done) => {
      if (!done) return;
      setText("");
      setFiles([]);
    });
  };
  const add = (adding: File[]) => {
    const refused = uploadLimit(adding, files.length);
    if (refused) report(refused);
    else setFiles((kept) => [...kept, ...adding]);
  };
  return (
    <form
      class="start-field message-box"
      onSubmit={submit}
      onDrop={(event) => {
        if (!attach || disabled || !event.dataTransfer?.files.length) return;
        event.preventDefault();
        add([...event.dataTransfer.files]);
      }}
    >
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
        onPaste={(event) => {
          const pasted = attach ? pastedFiles(event) : [];
          if (pasted.length) add(pasted);
        }}
      />
      {files.length > 0 && (
        <div class="attachments">
          {files.map((file, at) => (
            <FileChip
              key={at}
              name={file.name}
              remove={() =>
                setFiles((kept) => kept.filter((_, other) => other !== at))
              }
            >
              {file.name}
              <small>{bytes(file.size)}</small>
            </FileChip>
          ))}
        </div>
      )}
      <div class="start-tools message-actions">
        {attach && <AttachButton disabled={disabled} add={add} />}
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
