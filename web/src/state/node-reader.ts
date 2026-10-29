import { useCallback } from "preact/hooks";
import { readPages, routes } from "./api.ts";
import type { SessionRef } from "../shared/types.ts";

// Single node reader: custom read wins, else session detail endpoint.
export function useNodeReader(
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
            routes.session(
              session.id,
              `detail=${encodeURIComponent(id)}${raw ? "&raw=1" : ""}`,
            ),
            signal,
          ),
    [read, session.id],
  );
}
