import type { Session, Space } from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";
import { folderOf } from "../../shared/display.ts";
import { folderName } from "../../shared/folder-label.tsx";
import { plural } from "../../shared/quantities.ts";
import DeleteMany from "../../shared/delete-many.tsx";
import { deleteConversation } from "./conversation-actions.tsx";

// The sidebar's selection to the one delete confirmation: workspaces first,
// each taking its conversations, then the conversations outside them.

export default function DeleteConversations({
  sessions,
  spaces,
  online,
  done,
  close,
}: {
  sessions: Session[];
  spaces: Space[];
  online: boolean;
  // What went, by id or workspace path.
  done: (gone: string[]) => Promise<void>;
  close: () => void;
}) {
  const { manage, command } = useApi();
  // A conversation inside a workspace that goes anyway is not asked twice.
  const loose = sessions.filter(
    (item) => !spaces.some((space) => space.path === folderOf(item)),
  );
  const running = loose.filter((item) => item.generation).length;
  return (
    <DeleteMany
      entries={[
        ...spaces.map((space) => ({
          key: space.path,
          name: `Workspace “${space.name}”`,
          remove: () =>
            manage("space", { action: "delete", cwd: space.path, name: "" }),
        })),
        ...loose.map((item) => ({
          key: item.id,
          name: `${item.title || "Untitled"} · ${folderName(folderOf(item))}`,
          remove: () => deleteConversation(item, command),
        })),
      ]}
      note={
        <>
          {loose.length > 0 &&
            "Their saved messages, attachments and agents go; files they wrote in their folders stay. "}
          {spaces.length > 0 &&
            "A workspace's conversations and every file in it go too. "}
          {running > 0 &&
            `${plural(running, "open conversation")} close first. `}
        </>
      }
      online={online}
      done={done}
      close={close}
    />
  );
}
