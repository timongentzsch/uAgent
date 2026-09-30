import { useState } from "preact/hooks";
import type { SessionRef, TurnFile } from "../../shared/types.ts";
import { failure } from "../../shared/types.ts";
import { Actions, Button } from "../../shared/ui.tsx";
import { command } from "../../state/api.ts";
import { formatStat } from "./tool-preview.ts";

// A turn's changed files, each with its Undo. The host puts a file back
// only while it still holds what the turn left; otherwise it says why.
export default function TurnReview({
  files,
  turn,
  session,
}: {
  files: TurnFile[];
  turn: number;
  session: SessionRef;
}) {
  const [restored, setRestored] = useState<string[]>([]);
  const [kept, setKept] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState("");
  const undo = async (path?: string) => {
    setBusy(true);
    setNote("");
    try {
      const receipt = await command("revert", session, {
        turn,
        ...(path && { path }),
      });
      if (receipt.pending)
        throw new Error("Undo is still pending. Refresh to see its result.");
      const { restored: back, conflicts } = receipt.result;
      setRestored((prior) => [...prior, ...back]);
      setKept((prior) => ({
        ...prior,
        ...Object.fromEntries(
          conflicts.map((item) => [item.path, item.reason]),
        ),
      }));
      if (!back.length && !conflicts.length) setNote("Nothing left to undo.");
    } catch (error) {
      setNote(failure(error).message);
    } finally {
      setBusy(false);
    }
  };
  const open = files.filter(
    (file) => file.undoable && !restored.includes(file.path),
  );
  return (
    <>
      <p class="muted">
        Shell changes aren't tracked: only this turn's edits, writes and deletes
        can be undone.
      </p>
      <ul class="turn-files">
        {files.map((file) => (
          <li key={file.path}>
            <span class="turn-file-path" title={file.path}>
              {file.path}
            </span>
            <small>{formatStat([file.added, file.removed])}</small>
            {restored.includes(file.path) ? (
              <small>Restored</small>
            ) : file.undoable ? (
              <Button
                size="compact"
                aria-label={`Undo ${file.path}`}
                disabled={busy}
                onClick={() => void undo(file.path)}
              >
                Undo
              </Button>
            ) : (
              <small>Not kept</small>
            )}
            {kept[file.path] && (
              <p class="failure" role="alert">
                Kept {file.path}: {kept[file.path]}
              </p>
            )}
          </li>
        ))}
      </ul>
      {note && <p role="status">{note}</p>}
      <Actions>
        <Button
          variant="destructive"
          disabled={busy || !open.length}
          onClick={() => void undo()}
        >
          Undo all
        </Button>
      </Actions>
    </>
  );
}
