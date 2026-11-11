import {
  HostsContext,
  type Source,
  type HostDestination,
  type HostView,
  type SavedHost,
} from "../state/sources.ts";
import { type ComponentType } from "preact";
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import { ApiContext, hostApi, localApi } from "../state/api.ts";
import { useHost, type HostState } from "../state/use-host.ts";
import type { JSONValue } from "../shared/types.ts";
import { hostFromURL } from "../shared/navigation.ts";
import { hostsPollMs } from "../shared/limits.ts";
import { stored, useStored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { useLatest } from "../shared/use-latest.ts";

const saved = stored<SavedHost[]>(storageKeys.hosts, [], undefined, {
  kept: (hosts) =>
    hosts.map((host) => ({ ...host, phase: "disconnected", error: "" })),
});
type ResultHandler = (value: JSONValue, inspect: boolean) => void;
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
  const catalogue = useLatest(state.catalogue.sessions);
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
  // A device that signed out forgets the hosts it listed.
  useEffect(() => {
    if (state.authenticated === false) setHosts([]);
  }, [state.authenticated]);
  useEffect(() => {
    const changed = () => setActive(hostFromURL());
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
      if (!abort.signal.aborted) timer = setTimeout(read, hostsPollMs);
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
    (id: string, session = "", place = "", view: HostView = {}) => {
      // Sessions belong to the main catalogue, including notification links
      // that carry only a session id.
      const sessionOwner = catalogue.current.find(
        (item) => item.id === session,
      );
      if (sessionOwner) id = sessionOwner.host || "";
      if (
        gatewayOnline &&
        id &&
        hosts.find((host) => host.id === id)?.phase === "disconnected"
      )
        void action("connect", id).catch(state.report);
      // Selected at once: the address may be written later, under a layer.
      state.setSelected(session, id);
      setDestination({ id, session, place, ...view });
      setActive(id);
    },
    [hosts, action, state.report, state.setSelected, gatewayOnline],
  );
  // Keep filtered catalogue arrays stable while the composer changes its draft.
  const sources = useMemo<Source[]>(() => {
    const scope = (id: string) => ({
      listed: state.listed.filter((session) => (session.host || "") === id),
      catalogue: {
        ...state.catalogue,
        sessions: state.catalogue.sessions.filter(
          (session) => (session.host || "") === id,
        ),
        spaces: id ? [] : state.catalogue.spaces,
        pins: state.catalogue.pins?.filter((pin) => (pin.host || "") === id),
        terminals: state.catalogue.terminals?.filter(
          (terminal) => (terminal.host || "") === id,
        ),
      },
    });
    return [
      {
        id: "",
        name: "Local",
        address: location.host,
        phase: state.connection,
        error: "",
        state: scope(""),
      },
      ...hosts.map((host) => ({
        ...host,
        name: host.address,
        state: scope(host.id),
      })),
    ];
  }, [hosts, state.listed, state.catalogue, state.connection]);
  // The open conversation's host is the active one, whatever was asked for.
  const linkedSession = state.catalogue.sessions.find(
    (session) => session.id === state.selected,
  );
  const owner = linkedSession ? linkedSession.host || "" : active;
  const source = sources.find((item) => item.id === owner) || sources[0];
  // Cold notification links may lack a host. Preserve their decision target
  // while correcting the URL once the main catalogue identifies the session.
  useLayoutEffect(() => {
    if (!linkedSession || source.id !== owner) return;
    setActive(owner);
    const params = new URLSearchParams(location.hash.slice(1));
    // An address still being written carries its host itself.
    if (
      params.get("session") !== linkedSession.id ||
      (params.get("host") || "") === owner
    )
      return;
    if (owner) params.set("host", owner);
    else params.delete("host");
    history.replaceState(history.state, "", `#${params}`);
  }, [linkedSession, owner, source.id]);
  const context = useMemo(
    () => ({
      sources,
      active: source.id,
      gatewayOnline,
      open,
      action,
      destination,
    }),
    [sources, source.id, gatewayOnline, open, action, destination],
  );
  return (
    <HostsContext.Provider value={context}>
      <ApiContext.Provider value={hostApi(source.id)}>
        <App
          host={{ ...state, ...source.state }}
          bindResult={bindResult}
          reading={setReading}
        />
      </ApiContext.Provider>
    </HostsContext.Provider>
  );
}
