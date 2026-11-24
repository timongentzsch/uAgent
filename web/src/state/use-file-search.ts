import { fuzzy } from "../shared/fuzzy.ts";
import { useResource } from "../shared/use-resource.ts";
import { useApi } from "./api.ts";

export interface FileMatches {
  paths: string[];
  more: boolean;
}

export function useFileSearch(session: string, query?: string) {
  const { api, routes } = useApi();
  const search = useResource(
    (signal) =>
      query === undefined
        ? undefined
        : api<FileMatches>(routes.files(session, "find", query), undefined, {
            signal,
          }).then(({ paths, more }) => ({
            session,
            query,
            paths: fuzzy(paths, query, (path) => path),
            more,
          })),
    [session, query],
    undefined,
    query === undefined ? 0 : 150,
  );
  const { value } = search;
  return {
    value:
      value?.session === session && value.query === query ? value : undefined,
    error: search.error,
    retry: search.retry,
  };
}
