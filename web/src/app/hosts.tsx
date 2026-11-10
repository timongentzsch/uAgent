import {
  HostsContext,
  type Source,
  type HostDestination,
  type SavedHost,
} from "../state/sources.ts";
export { useHosts } from "../state/sources.ts";
import { type ComponentType } from "preact";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { ApiContext, hostApi, localApi } from "../state/api.ts";
import { useHost, type HostState } from "../state/use-host.ts";
import type { JSONValue } from "../shared/types.ts";
import { hostFromURL, writeSelection } from "../shared/navigation.ts";
import { stored, useStored } from "../shared/storage.ts";

const saved = stored<SavedHost[]>("uagent-hosts", [], undefined, {
  kept: (hosts) =>
    hosts.map((host) => ({ ...host, phase: "disconnected", error: "" })),
});
export type ResultHandler = (value: JSONValue, inspect: boolean) => void;
export interface HostAppProps {
  host: HostState;
  bindResult: (handler: ResultHandler) => () => void;
  reading: (value: boolean) => void;
}
export default function HostsShell({
  App,
}: {
  App: ComponentType<HostAppProps>;
}) {
  const result = useRef<ResultHandler>(() => {});
  const deliver = useCallback<ResultHandler>(
    (value, inspect) => result.current(value, inspect),
    [],
  );
  const [reading, setReading] = useState(true);
  // One main-owned catalogue and stream, including offline remote history.
  const state = useHost(deliver, reading);
  const bindResult = useCallback((handler: ResultHandler) => {
    result.current = handler;
    return () => {
      result.current = () => {};
    };
  }, []);
  const [hosts, setHosts] = useStored(saved);
  const [active, setActive] = useState(hostFromURL);
  const [destination, setDestination] = useState<HostDestination | null>(null);
  const gatewayOnline = state.online;
  const scoped = useCallback(
    (id: string): HostState => ({
      ...state,
      listed: state.listed.filter((session) => (session.host || "") === id),
      catalogue: {
        ...state.catalogue,
        sessions: state.catalogue.sessions.filter(
          (session) => (session.host || "") === id,
        ),
        spaces: id ? [] : state.catalogue.spaces,
        pins: state.catalogue.pins?.filter((pin) => (pin.host || "") === id),
        terminals: state.catalogue.terminals?.filter(
          (terminal) => ((terminal as { host?: string }).host || "") === id,
        ),
      },
    }),
    [state],
  );
  useEffect(() => {
    const changed = () => {
      setDestination(null);
      setActive(hostFromURL());
    };
    addEventListener("hashchange", changed);
    addEventListener("popstate", changed);
    return () => {
      removeEventListener("hashchange", changed);
      removeEventListener("popstate", changed);
    };
  }, []);
  useEffect(() => {
    if (!gatewayOnline) return;
    const abort = new AbortController();
    let timer: ReturnType<typeof setTimeout>;
    const read = async () => {
      try {
        const result = await localApi.api<{ hosts: SavedHost[] }>(
          localApi.routes.hosts,
          undefined,
          { signal: abort.signal },
        );
        if (!abort.signal.aborted)
          setHosts((prior) =>
            JSON.stringify(prior) === JSON.stringify(result.hosts)
              ? prior
              : result.hosts,
          );
      } catch {
        /* Local's stream owns the gateway connection status. */
      }
      if (!abort.signal.aborted) timer = setTimeout(read, 2000);
    };
    read();
    return () => {
      abort.abort();
      clearTimeout(timer);
    };
  }, [gatewayOnline]);
  const action = useCallback(
    async (action: string, id?: string, address?: string) => {
      const result = await localApi.api<{ id: string }>(localApi.routes.hosts, {
        action,
        id,
        address,
      });
      const fresh = await localApi.api<{ hosts: SavedHost[] }>(
        localApi.routes.hosts,
      );
      setHosts(fresh.hosts);
      return result.id;
    },
    [],
  );
  const open = useCallback(
    (
      id: string,
      session = "",
      place = "",
      view: { terminal?: string; settings?: string } = {},
    ) => {
      if (id && hosts.find((host) => host.id === id)?.phase === "disconnected")
        void action("connect", id).catch((error) => state.report(error));
      writeSelection(session, false, id);
      if (place) {
        const params = new URLSearchParams(location.hash.slice(1));
        params.set("place", place);
        history.replaceState(history.state, "", `#${params}`);
      }
      setDestination({ id, session, place, ...view });
      setActive(id);
    },
    [hosts, action, state.report],
  );
  const sources = useMemo<Source[]>(
    () => [
      {
        id: "",
        name: "Local",
        address: location.host,
        phase: state.connection,
        error: "",
        state: scoped(""),
      },
      ...hosts.map((host) => ({ ...host, state: scoped(host.id) })),
    ],
    [hosts, state, scoped],
  );
  const context = useMemo(
    () => ({ sources, active, gatewayOnline, open, action, destination }),
    [sources, active, gatewayOnline, open, action, destination],
  );
  const selected = sources.some((source) => source.id === active) ? active : "";
  return (
    <HostsContext.Provider value={context}>
      <ApiContext.Provider value={hostApi(selected)}>
        <App
          host={scoped(selected)}
          bindResult={bindResult}
          reading={setReading}
        />
      </ApiContext.Provider>
    </HostsContext.Provider>
  );
}
