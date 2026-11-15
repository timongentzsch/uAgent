import { useState } from "preact/hooks";
import type { Session, Space } from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
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
  Notice,
} from "../../shared/ui.tsx";

// The conversation and the agents under it, which a move takes along.
function familyOf(sessions: Session[], item: Session) {
  const family = [item];
  for (const member of family)
    family.push(...sessions.filter((other) => other.parent === member.id));
  return family;
}

export default function MoveConversation({
  moving,
  sessions,
  spaces,
  online,
  close,
}: {
  moving: Session[];
  sessions: Session[];
  spaces: Space[];
  online: boolean;
  close: () => void;
}) {
  const { command } = useApi();
  const [query, setQuery] = useState("");
  const [chosen, setChosen] = useState("");
  const { run, busy, error } = useAction();
  const places = new Set(moving.map(folderOf));
  const current = places.size === 1 ? [...places][0] : "";
  const family = moving.flatMap((item) => familyOf(sessions, item));
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
          for (const item of moving)
            if (folderOf(item) !== target)
              await command("move", item, { cwd: target });
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
        {moving.length > 1
          ? `Moves ${plural(moving.length, "conversation")}`
          : "Moves the conversation"}
        {family.length > moving.length &&
          ` and ${plural(family.length - moving.length, "agent")}`}
        {`. Files stay ${current ? `in ${current}` : "where they are"}, and memories stay with the old ${current ? "folder" : "folders"}.`}
      </p>
      {running.length > 0 && (
        <Notice tone="warning">
          It cannot move while{" "}
          {running.map((item) => `“${item.title}”`).join(", ")}{" "}
          {running.length === 1 ? "is" : "are"} running.
        </Notice>
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
