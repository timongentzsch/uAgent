import { useCallback } from "preact/hooks";
import { readPages } from "./api.ts";
import type { SessionRef } from "../shared/types.ts";

// Single block reader: custom read wins, else session detail endpoint.
export function useBlockReader(
  session: SessionRef,
  read?: (
    id: string,
    raw: boolean,
    signal?: AbortSignal,
  ) => Promise<{ text: string }>,
) {
  return useCallback(
    (id: string, raw: boolean, signal?: AbortSignal) =>
      read
        ? read(id, raw, signal)
        : readPages(
            `/api/sessions/${session.id}?detail=${encodeURIComponent(id)}${raw ? "&raw=1" : ""}`,
            signal,
          ),
    [read, session.id],
  );
}
