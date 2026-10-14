import { useState } from "preact/hooks";
import type { StatisticsModal } from "../../shared/types.ts";
import { command, manage } from "../../state/api.ts";
import { useAction } from "../../shared/use-action.ts";
import { deleteRetryDelayMs } from "../../shared/limits.ts";
import { plural } from "../../shared/quantities.ts";
import { Actions, Button, LoadError, Input } from "../../shared/ui.tsx";
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
  const [title, setTitle] = useState(
    modal.space?.name || modal.session?.title || "",
  );
  const { run, busy, error } = useAction();
  return (
    <form
      onSubmit={(event) => {
        event.preventDefault();
        void run(async () => {
          if (modal.space) {
            // The host does the whole of it: a workspace's conversations close
            // and go with it.
            await manage("space", {
              action: modal.type,
              cwd: modal.space.path,
              name: title.trim(),
            });
            await changed(modal.type, "");
            close();
            return;
          }
          if (modal.type === "delete" && modal.session.generation) {
            // Live sessions close first: close aborts a running turn
            // gracefully, then delete removes the record. A closing worker
            // can retire while deactivation propagates, so delete retries
            // until the generation clears server-side.
            try {
              await command("close", modal.session);
            } catch (failure) {
              // Nothing to stop (already closed, or owned elsewhere):
              // only a stale-worker rejection falls through to delete;
              // anything else aborts with the real error.
              if (!/stale worker generation/i.test(String(failure)))
                throw failure;
            }
            const closed = { ...modal.session, generation: "" };
            let lastError: unknown = null;
            for (let attempt = 0; attempt < 4; attempt++) {
              try {
                await command("delete", closed);
                lastError = null;
                break;
              } catch (failure) {
                lastError = failure;
                if (!/stop and close/i.test(String(failure))) break;
                await new Promise((resolve) =>
                  setTimeout(resolve, deleteRetryDelayMs),
                );
              }
            }
            if (lastError) throw lastError;
          } else {
            await command(
              modal.type,
              modal.session,
              modal.type === "rename" ? { title: title.trim() } : {},
            );
          }
          await changed(modal.type, modal.session.id);
          close();
        });
      }}
    >
      {modal.type === "rename" ? (
        <label>
          {modal.space
            ? "Workspace name"
            : modal.session.kind === "panel"
              ? "Moderator’s name"
              : modal.session.member
                ? "Member’s name"
                : "Conversation name"}
          <Input
            autoFocus
            value={title}
            onInput={(event) => setTitle(event.currentTarget.value)}
            required
            maxLength={modal.space ? 64 : 256}
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
          Project files stay in place. This cannot be undone.{" "}
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
