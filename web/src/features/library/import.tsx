import { manage } from "../../state/api.ts";
import type { ImportItem } from "../../shared/types.ts";
import { useState } from "preact/hooks";
import {
  Button,
  EmptyState,
  LoadError,
  Modal,
  Row,
  Spinner,
} from "../../shared/ui.tsx";
import { bytes } from "../../shared/quantities.ts";
import { useResource } from "../../shared/use-resource.ts";

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
  const list = () =>
    manage("import", { cwd, action: "list", only }).then(
      (result) => result.items || [],
    );
  // An import's own answer is the list read after it.
  const {
    value: items,
    error,
    setValue,
    setError,
  } = useResource(list, [cwd, only]);
  const [busy, setBusy] = useState("");
  async function run(item: ImportItem) {
    setBusy(item.key);
    setError(null);
    try {
      await manage("import", { cwd, action: "run", only, key: item.key });
      setValue(await list());
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
            <Row
              key={item.key}
              label={item.name}
              detail={`${item.source} · ${bytes(item.bytes)}${note ? ` · ${note}` : ""}`}
            >
              {!note && (
                <Button
                  disabled={!!busy}
                  busy={busy === item.key}
                  onClick={() => run(item)}
                >
                  Import
                </Button>
              )}
            </Row>
          );
        })
      ) : (
        !error && (
          <EmptyState>
            Nothing from Claude Code or Codex to import for this folder.
          </EmptyState>
        )
      )}
      {error && <LoadError error={error} />}
    </Modal>
  );
}
