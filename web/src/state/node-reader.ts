import { useCallback } from "preact/hooks";
import { useApi } from "./api.ts";
import type { SessionRef } from "../shared/types.ts";

// Single node reader: the session's detail endpoint.
export function useNodeReader(session: SessionRef) {
  const { readPages, routes } = useApi();
  return useCallback(
    (id: string, raw: boolean, signal?: AbortSignal) =>
      readPages(
        routes.session(
          session.id,
          `detail=${encodeURIComponent(id)}${raw ? "&raw=1" : ""}`,
        ),
        signal,
      ),
    [session.id, routes.session, readPages],
  );
}
