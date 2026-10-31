import { useState } from "preact/hooks";
import type { Session, Space } from "../../shared/types.ts";
import { command } from "../../state/api.ts";
import { folderOf } from "../../shared/display.ts";
import { matches } from "../../shared/fuzzy.ts";
import { plural } from "../../shared/quantities.ts";
import { useAction } from "../../shared/use-action.ts";
import FolderLabel from "../../shared/folder-label.tsx";
import { ListRow } from "../../shared/list-row.tsx";
import {
  Actions,
  Button,
  EmptyState,
  Field,
  Group,
  Input,
  LoadError,
} from "../../shared/ui.tsx";

// The conversation and the agents under it, which a move takes along.
export function familyOf(sessions: Session[], item: Session) {
  const family = [item];
  for (const member of family)
    family.push(...sessions.filter((other) => other.parent === member.id));
  return family;
}

// The dialog body that moves a conversation to another folder or workspace:
// a place picked from the list or typed as a path.
export default function MoveConversation({
  session,
  sessions,
  spaces,
  online,
  close,
}: {
  // As the catalogue holds it now, so the dialog follows what runs.
  session: Session;
  sessions: Session[];
  spaces: Space[];
  online: boolean;
  close: () => void;
}) {
  const [query, setQuery] = useState("");
  const [chosen, setChosen] = useState("");
  const { run, busy, error } = useAction();
  const current = folderOf(session);
  const family = familyOf(sessions, session);
  const running = family.filter((item) => item.turn_active || item.pending);
  const typed = query.trim().startsWith("/") ? query.trim() : "";
  const target = chosen || typed;
  const folders = [
    ...new Set(
      [...sessions]
        .sort((a, b) => (b.updated || 0) - (a.updated || 0))
        .map(folderOf)
        .filter((path) => path && !spaces.some((space) => space.path === path)),
    ),
  ];
  const place = (path: string, name?: string) => (
    <ListRow
      key={path}
      title={
        <FolderLabel path={path} space={!!name}>
          {name}
        </FolderLabel>
      }
      meta={path === current ? "Current" : undefined}
      aria-current={path === chosen ? "true" : undefined}
      disabled={path === current || busy}
      onClick={() => setChosen(path)}
    />
  );
  const workspaces = spaces.filter((space) =>
    matches(query, space.name, space.path),
  );
  const shown = folders.filter((path) => matches(query, path));
  return (
    <form
      onSubmit={(event) => {
        event.preventDefault();
        // The page follows the conversation to its new id when the host
        // announces the move, here and on every other device.
        void run(async () => {
          await command("move", session, { cwd: target });
          close();
        });
      }}
    >
      <Field label="Place">
        <Input
          autoFocus
          value={query}
          onInput={(event) => {
            setQuery(event.currentTarget.value);
            setChosen("");
          }}
          placeholder="Filter, or type a folder path"
          autoComplete="off"
        />
      </Field>
      {workspaces.length > 0 && (
        <Group title="Workspaces">
          {workspaces.map((space) => place(space.path, space.name))}
        </Group>
      )}
      {shown.length > 0 && (
        <Group title="Folders">{shown.map((path) => place(path))}</Group>
      )}
      {!workspaces.length && !shown.length && (
        <EmptyState>
          {typed
            ? `Moves to ${typed}.`
            : "No place matches. Type the path of a folder on the host."}
        </EmptyState>
      )}
      <p class="muted">
        Moves the conversation
        {family.length > 1 && ` and its ${plural(family.length - 1, "agent")}`}.
        Files stay in {current}, and memories stay with the old folder.
      </p>
      {running.length > 0 && (
        <p role="status">
          It cannot move while{" "}
          {running.map((item) => `“${item.title}”`).join(", ")}{" "}
          {running.length === 1 ? "is" : "are"} running.
        </p>
      )}
      {error && <LoadError error={error} />}
      <Actions>
        <Button onClick={close}>Cancel</Button>
        <Button
          type="submit"
          variant="primary"
          disabled={
            !online ||
            busy ||
            running.length > 0 ||
            !target ||
            target === current
          }
        >
          Move
        </Button>
      </Actions>
    </form>
  );
}
