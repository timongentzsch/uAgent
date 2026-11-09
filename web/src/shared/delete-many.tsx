import { useState } from "preact/hooks";
import type { ComponentChildren } from "preact";
import { plural } from "./quantities.ts";
import { Actions, Button } from "./ui.tsx";

// The one confirmation for deleting what a list has selected (the sidebar's
// and the Library's): it names them, says what goes and what stays, then
// deletes each the way its own menu does, one after another. What could not
// be deleted is said, with why, and stays selected.

export interface Doomed {
  // The selection's id for it.
  key: string;
  name: string;
  remove: () => Promise<unknown>;
}

// "Delete 1 deck, 1 doc and 2 memories?" from [count, noun, plural?] parts.
export const deleteTitle = (parts: [number, string, string?][]) => {
  const named = parts
    .filter(([count]) => count)
    .map(([count, noun, many]) => plural(count, noun, many));
  const last = named.pop();
  return `Delete ${named.length ? `${named.join(", ")} and ${last}` : last}?`;
};

// The names a dialog lists before "and N more".
const LISTED = 6;

export default function DeleteMany({
  entries,
  note,
  online,
  done,
  close,
}: {
  entries: Doomed[];
  // What goes and what stays; "This cannot be undone." follows.
  note: ComponentChildren;
  online: boolean;
  // The keys that went, once all were tried.
  done: (gone: string[]) => unknown;
  close: () => void;
}) {
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(0);
  const [failed, setFailed] = useState<{ name: string; why: string }[]>();
  const remove = async () => {
    setBusy(true);
    const gone: string[] = [];
    const wrong: { name: string; why: string }[] = [];
    for (const entry of entries) {
      try {
        await entry.remove();
        gone.push(entry.key);
      } catch (error) {
        wrong.push({
          name: entry.name,
          why: String((error as Error)?.message ?? error),
        });
      }
      setProgress((count) => count + 1);
    }
    await done(gone);
    setBusy(false);
    if (wrong.length) setFailed(wrong);
    else close();
  };
  if (failed)
    return (
      <>
        <p>{`Deleted ${entries.length - failed.length} of ${entries.length}. These stay selected:`}</p>
        <ul>
          {failed.map((item) => (
            <li key={item.name}>
              {item.name}: <span class="muted">{item.why}</span>
            </li>
          ))}
        </ul>
        <Actions>
          <Button variant="primary" onClick={close}>
            OK
          </Button>
        </Actions>
      </>
    );
  return (
    <form
      onSubmit={(event) => {
        event.preventDefault();
        void remove();
      }}
    >
      <ul>
        {entries.slice(0, LISTED).map((entry) => (
          <li key={entry.key}>{entry.name}</li>
        ))}
        {entries.length > LISTED && (
          <li class="muted">{`and ${entries.length - LISTED} more`}</li>
        )}
      </ul>
      <p>
        {note}
        This cannot be undone.
      </p>
      <Actions>
        <Button onClick={close} disabled={busy}>
          Cancel
        </Button>
        <Button
          type="submit"
          variant="destructive"
          disabled={!online || busy}
          busy={busy}
        >
          {busy
            ? `Deleting ${progress + 1} of ${entries.length}…`
            : `Delete ${entries.length}`}
        </Button>
      </Actions>
    </form>
  );
}
