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
          if (modal.type === "delete" && modal.session.generation) {
            // Live sessions close first: close aborts a running turn
            // gracefully, then delete removes the record. Close resolves
            // once the worker exits, but deactivation (which clears the
            // generation server-side) can land a beat later, so delete
            // follows with a cleared generation and retries a racing
            // rejection bounded instead of surfacing it.
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
                await command("delete", closed, {});
                lastError = null;
                break;
              } catch (failure) {
                lastError = failure;
                if (!/stop and close/i.test(String(failure))) break;
                await new Promise((resolve) => setTimeout(resolve, 1000));
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
            // eslint-disable-next-line jsx-a11y/no-autofocus
            autoFocus
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
          Project files stay in place. This cannot be undone.{" "}
          {modal.session.generation
            ? modal.session.turn_active
              ? "The running turn stops first."
              : "The open session closes first."
            : ""}
        </p>
      )}
      <div class="dialog-actions">
        <button type="button" onClick={close}>
          Cancel
        </button>
        <button
          class="primary"
          disabled={
            !online || busy || (modal.type === "rename" && !title.trim())
          }
        >
          {modal.type === "rename" ? "Save name" : "Delete permanently"}
        </button>
      </div>
    </form>
  );
}
