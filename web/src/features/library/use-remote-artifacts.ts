import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import {
  failure,
  type LibraryItem,
  type LibraryPlace,
} from "../../shared/types.ts";
import { useApi } from "../../state/api.ts";

type Listing = { items?: LibraryItem[]; error?: string };

// Each SSH workspace owns its read. Retrying or adding one workspace leaves
// the other hosts and any outstanding reads alone.
export function useRemoteArtifacts(places: LibraryPlace[], online: boolean) {
  const { api, routes } = useApi();
  const [results, setResults] = useState<Record<string, Listing>>({});
  const requests = useRef(new Map<string, AbortController>());
  const load = useCallback(
    (id: string) => {
      if (!online) return;
      requests.current.get(id)?.abort();
      const controller = new AbortController();
      requests.current.set(id, controller);
      const receive = (value: Listing) => {
        if (!controller.signal.aborted)
          setResults((all) => ({ ...all, [id]: value }));
      };
      void api<{ items: LibraryItem[] }>(
        `${routes.library}?session=${encodeURIComponent(id)}`,
        undefined,
        { signal: controller.signal },
      ).then(receive, (error) => receive({ error: failure(error).message }));
    },
    [api, routes.library, online],
  );
  // Abort obsolete reads when changing server, going offline or unmounting.
  useEffect(() => {
    const active = requests.current;
    return () => {
      for (const controller of active.values()) controller.abort();
      active.clear();
    };
  }, [load]);
  useEffect(() => {
    const ids = new Set(places.flatMap((place) => place.session ?? []));
    for (const [id, controller] of requests.current) {
      if (!ids.has(id)) {
        controller.abort();
        requests.current.delete(id);
      }
    }
    setResults((all) =>
      Object.keys(all).some((id) => !ids.has(id))
        ? Object.fromEntries(Object.entries(all).filter(([id]) => ids.has(id)))
        : all,
    );
    if (!online) return;
    for (const id of ids) {
      if (!requests.current.has(id)) load(id);
    }
  }, [online, places, load]);
  return { results, retry: load };
}
