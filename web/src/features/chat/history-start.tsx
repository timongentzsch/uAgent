import type { View } from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import "./chat.css";

// The top of a transcript: the button that loads older rows.

export default function HistoryStart({
  view,
  online,
  loading,
  load,
}: {
  view?: View;
  online: boolean;
  loading: boolean;
  load: () => void;
}) {
  return (
    <>
      {view?.more && (
        <Button
          variant="quiet"
          class="history-button"
          disabled={!online}
          busy={loading}
          onClick={load}
        >
          {loading ? "Loading older messages…" : "Load older retained messages"}
        </Button>
      )}
      {(view?.dropped_segments || 0) > 0 && (
        <p class="retention small">
          {view?.dropped_segments} older segments are outside retention.
        </p>
      )}
    </>
  );
}
