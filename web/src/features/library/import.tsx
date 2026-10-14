import { manage } from "../../state/api.ts";
import type { ImportItem } from "../../shared/types.ts";
import { useCallback, useEffect, useState } from "preact/hooks";
import {
  Button,
  DataText,
  EmptyState,
  LoadError,
  Modal,
  Spinner,
} from "../../shared/ui.tsx";
import { bytes } from "../../shared/quantities.ts";

// What Claude Code and Codex keep that can come into µAgent's library.
export default function ImportDialog({
  cwd,
  kind,
  close,
}: {
  cwd: string;
  kind: "memory" | "skills";
  close: () => void;
}) {
  const only = kind === "skills" ? "skill" : "memory";
  const [items, setItems] = useState<ImportItem[] | null>(null);
  const [busy, setBusy] = useState("");
  const [error, setError] = useState<unknown>(null);
  const load = useCallback(async () => {
    try {
      const result = await manage("import", { cwd, action: "list", only });
      setItems(result.items || []);
    } catch (failure) {
      setError(failure);
    }
  }, [cwd, only]);
  useEffect(() => {
    void load();
  }, [load]);
  async function run(item: ImportItem) {
    setBusy(item.key);
    setError(null);
    try {
      await manage("import", { cwd, action: "run", only, key: item.key });
      await load();
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy("");
    }
  }
  return (
    <Modal title="Import from Claude Code and Codex" close={close}>
      {!items && !error ? (
        <Spinner label="Looking for importable items…" />
      ) : items?.length ? (
        items.map((item) => {
          const note =
            item.blocked ||
            (item.collides && "Already in µAgent") ||
            (item.needs_model && "Needs a model: invoke $import in a chat");
          return (
            <div class="import-row" key={item.key}>
              <span>
                <DataText>{item.name}</DataText>
                <small>
                  {item.source} · {bytes(item.bytes)}
                  {note ? ` · ${note}` : ""}
                </small>
              </span>
              {!note && (
                <Button
                  disabled={!!busy}
                  busy={busy === item.key}
                  onClick={() => run(item)}
                >
                  Import
                </Button>
              )}
            </div>
          );
        })
      ) : (
        !error && (
          <EmptyState>
            Nothing from Claude Code or Codex to import for this project.
          </EmptyState>
        )
      )}
      {error && <LoadError error={error} />}
    </Modal>
  );
}
