import { useState } from "preact/hooks";
import type { StatisticsModal } from "../../shared/types.ts";
import { command } from "../../state/api.ts";
import { LoadError } from "../../shared/ui.tsx";
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
  const [title, setTitle] = useState(modal.session.title || "");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  return (
    <form
      onSubmit={async (event) => {
        event.preventDefault();
        setBusy(true);
        setError(null);
        try {
          await command(
            modal.type,
            modal.session,
            modal.type === "rename" ? { title: title.trim() } : {},
          );
          await changed(modal.type, modal.session.id);
          close();
        } catch (error) {
          setError(error);
        } finally {
          setBusy(false);
        }
      }}
    >
      {error && <LoadError error={error} />}
      {modal.type === "rename" ? (
        <label>
          Conversation name
          <input
            value={title}
            onInput={(event) => setTitle(event.currentTarget.value)}
            required
            maxLength={256}
            autoComplete="off"
          />
        </label>
      ) : (
        <p>
          Delete “{modal.session.title}” and its saved messages and attachments?
          Project files stay in place. This cannot be undone.
        </p>
      )}
      <div class="dialog-actions">
        <button
          class="primary"
          disabled={
            !online || busy || (modal.type === "rename" && !title.trim())
          }
        >
          {modal.type === "rename" ? "Save name" : "Delete permanently"}
        </button>
        <button type="button" onClick={close}>
          Cancel
        </button>
      </div>
    </form>
  );
}
