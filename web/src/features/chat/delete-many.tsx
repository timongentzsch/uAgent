import { useState } from "preact/hooks";
import type { Session, Space } from "../../shared/types.ts";
import { manage } from "../../state/api.ts";
import { folderOf } from "../../shared/display.ts";
import { folderName } from "../../shared/folder-label.tsx";
import { plural } from "../../shared/quantities.ts";
import { Actions, Button } from "../../shared/ui.tsx";
import { deleteConversation } from "./conversation-actions.tsx";

// The dialog body that deletes what the sidebar has selected: conversations
// and workspaces, each the way its own menu does it, one after another. A
// workspace takes its conversations with it. What could not be deleted is
// said, with why, and stays selected.

// The names a dialog lists before "and N more".
const LISTED = 6;

export default function DeleteMany({
  sessions,
  spaces,
  online,
  done,
  close,
}: {
  sessions: Session[];
  spaces: Space[];
  online: boolean;
  // What went, by id or workspace path, once all were tried.
  done: (gone: string[]) => Promise<void>;
  close: () => void;
}) {
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(0);
  const [failed, setFailed] = useState<{ name: string; why: string }[]>();
  // A conversation inside a workspace that goes anyway is not asked twice.
  const loose = sessions.filter(
    (item) => !spaces.some((space) => space.path === folderOf(item)),
  );
  const total = spaces.length + loose.length;
  const running = loose.filter((item) => item.generation).length;
  const names = [
    ...spaces.map((space) => `Workspace “${space.name}”`),
    ...loose.map(
      (item) => `${item.title || "Untitled"} · ${folderName(folderOf(item))}`,
    ),
  ];
  const remove = async () => {
    setBusy(true);
    const gone: string[] = [];
    const wrong: { name: string; why: string }[] = [];
    const attempt = async (key: string, name: string, work: () => unknown) => {
      try {
        await work();
        gone.push(key);
      } catch (error) {
        wrong.push({ name, why: String((error as Error)?.message ?? error) });
      }
      setProgress((count) => count + 1);
    };
    for (const space of spaces)
      await attempt(space.path, `Workspace “${space.name}”`, () =>
        manage("space", { action: "delete", cwd: space.path, name: "" }),
      );
    for (const item of loose)
      await attempt(item.id, item.title || "Untitled", () =>
        deleteConversation(item),
      );
    await done(gone);
    setBusy(false);
    if (wrong.length) setFailed(wrong);
    else close();
  };
  if (failed)
    return (
      <>
        <p>{`Deleted ${total - failed.length} of ${total}. These stay selected:`}</p>
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
        {names.slice(0, LISTED).map((name) => (
          <li key={name}>{name}</li>
        ))}
        {names.length > LISTED && (
          <li class="muted">{`and ${names.length - LISTED} more`}</li>
        )}
      </ul>
      <p>
        {loose.length > 0 &&
          "Their saved messages, attachments and agents go; files they wrote in their folders stay. "}
        {spaces.length > 0 &&
          "A workspace's conversations and every file in it go too. "}
        {running > 0 && `${plural(running, "open conversation")} close first. `}
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
          {busy ? `Deleting ${progress + 1} of ${total}…` : `Delete ${total}`}
        </Button>
      </Actions>
    </form>
  );
}
