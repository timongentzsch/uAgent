import { useState } from "preact/hooks";
import type { Session, StatisticsModal } from "../../shared/types.ts";
import { useApi, type ApiClient } from "../../state/api.ts";
import { useAction } from "../../shared/use-action.ts";
import {
  deleteRetryDelayMs,
  maxSpaceNameChars,
  maxTitleChars,
} from "../../shared/limits.ts";
import { plural } from "../../shared/quantities.ts";
import { Actions, Button, LoadError, Input } from "../../shared/ui.tsx";

// A live one closes first. A closing worker can retire while that spreads,
// so delete retries until the host has let go of it.
export async function deleteConversation(
  session: Session,
  command: ApiClient["command"],
) {
  if (!session.generation) return void (await command("delete", session));
  try {
    await command("close", session);
  } catch (failure) {
    // Nothing to stop (already closed, or owned elsewhere): only a
    // stale-worker refusal goes on to delete; anything else is the error.
    if (!/stale worker generation/i.test(String(failure))) throw failure;
  }
  const closed = { ...session, generation: "" };
  let lastError: unknown = null;
  for (let attempt = 0; attempt < 4; attempt++) {
    try {
      await command("delete", closed);
      return;
    } catch (failure) {
      lastError = failure;
      if (!/stop and close/i.test(String(failure))) break;
      await new Promise((resolve) => setTimeout(resolve, deleteRetryDelayMs));
    }
  }
  throw lastError;
}

export default function ConversationActions({
  modal,
  online,
  changed,
  close,
}: {
  modal: Extract<StatisticsModal, { type: "rename" | "delete" }>;
  online: boolean;
  changed: (kind: string, id: string) => Promise<void>;
  close: () => void;
}) {
  const { manage, command } = useApi();
  const [title, setTitle] = useState(
    modal.space?.name || modal.session?.title || modal.terminal?.title || "",
  );
  const { run, busy, error } = useAction();
  return (
    <form
      onSubmit={(event) => {
        event.preventDefault();
        void run(async () => {
          if (modal.terminal) {
            await manage("terminal", {
              action: "rename",
              id: modal.terminal.id,
              title: title.trim(),
            });
            close();
            return;
          }
          if (modal.space) {
            // The host closes and removes a workspace's conversations with it.
            await manage("space", {
              action: modal.type,
              cwd: modal.space.path,
              name: title.trim(),
            });
            await changed(modal.type, "");
            close();
            return;
          }
          if (modal.type === "delete")
            await deleteConversation(modal.session, command);
          else await command("rename", modal.session, { title: title.trim() });
          await changed(modal.type, modal.session.id);
          close();
        });
      }}
    >
      {modal.type === "rename" ? (
        <label>
          {modal.space
            ? "Workspace name"
            : modal.terminal
              ? "Terminal name"
              : "Conversation name"}
          <Input
            autoFocus
            value={title}
            onInput={(event) => setTitle(event.currentTarget.value)}
            required
            maxLength={modal.space ? maxSpaceNameChars : maxTitleChars}
            autoComplete="off"
          />
        </label>
      ) : modal.space ? (
        <p>
          Delete the workspace “{modal.space.name}”
          {modal.conversations
            ? `, its ${plural(modal.conversations, "conversation")}`
            : ""}{" "}
          and every file in it? This cannot be undone.
        </p>
      ) : (
        <p>
          Delete “{modal.session.title}” and its saved messages and attachments?
          Files in the folder stay in place. This cannot be undone.{" "}
          {modal.session.generation
            ? modal.session.turn_active
              ? "The running turn stops first."
              : "The open conversation closes first."
            : ""}
        </p>
      )}
      {error && <LoadError error={error} />}
      <Actions>
        <Button onClick={close}>Cancel</Button>
        <Button
          type="submit"
          variant={modal.type === "delete" ? "destructive" : "primary"}
          disabled={
            !online || busy || (modal.type === "rename" && !title.trim())
          }
        >
          {modal.type === "rename" ? "Save name" : "Delete permanently"}
        </Button>
      </Actions>
    </form>
  );
}
