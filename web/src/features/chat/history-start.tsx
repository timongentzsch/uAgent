import type { View } from "../../shared/types.ts";

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
        <button
          type="button"
          class="quiet history-button"
          disabled={!online || loading}
          aria-busy={loading || undefined}
          onClick={load}
        >
          {loading ? "Loading older messages…" : "Load older retained messages"}
        </button>
      )}
      {(view?.dropped_segments || 0) > 0 && (
        <p class="retention">
          {view?.dropped_segments} older segments are outside retention.
        </p>
      )}
    </>
  );
}
