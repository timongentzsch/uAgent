import type {
  JSONValue,
  Snapshot,
  Catalogue,
  Draft,
  Outgoing,
  HostEvent,
  Outcome,
} from "./types.ts";
import { failure } from "./types.ts";
import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import {
  isAttention,
  retainedViews,
  applySessionEvent,
  isIncoming,
  mergeCached,
  readStored,
  writeStored,
} from "./store.ts";
import { api, protocol, receiveOutcome } from "./api.ts";
import { selectedFromURL, writeSelection } from "./navigation.ts";

// One SSE subscription owns host snapshots, command receipts and read state.
export function useHost(
  onResult: (value: JSONValue, inspect: boolean) => void,
  readingConversation = true,
) {
  const [managementVersion, setManagementVersion] = useState(0);
  const [authenticated, setAuthenticated] = useState<boolean | null>(null);
  const [online, setOnline] = useState(false);
  const [connecting, setConnecting] = useState(true);
  const [loadErrors, setLoadErrors] = useState<Record<string, unknown>>({});
  const revoked = useRef(false);
  const catalogueRef = useRef<Catalogue>();
  const loads = useRef(new Map<string, Promise<Snapshot>>());
  const lifetime = useRef(new AbortController());
  const [catalogue, setCatalogue] = useState<Catalogue>({
    sessions: [],
    capabilities: {},
    devices: [],
  });
  const [snapshots, setSnapshots] = useState<Record<string, Snapshot>>({});
  const [selected, setSelected] = useState(selectedFromURL);
  const [drafts, setDrafts] = useState<Record<string, Draft>>({});
  const [error, setError] = useState("");
  const [unread, setUnread] = useState(
    () => new Set(readStored<string[]>(localStorage, "uagent-unread", [])),
  );
  const [outgoing, setOutgoing] = useState(() =>
    readStored<Outgoing[]>(sessionStorage, "uagent-outgoing", []),
  );
  const [following, setFollowing] = useState(true);
  const stream = useRef<EventSource>();
  const reconnecting = useRef(false);
  const refreshAgain = useRef(false);
  const live = useRef<Record<string, Snapshot>>({});
  const selection = useRef(selected);
  const flush = useRef((_id?: string) => {});
  const notifications = useRef(false);
  const subscribed = useRef(false);
  const reading = useRef(false);
  const readCounts = useRef(
    readStored<Record<string, number>>(localStorage, "uagent-read", {}),
  );
  const outgoingRef = useRef(outgoing);
  const localRequests = useRef(new Set<string>());
  subscribed.current = !!catalogue.capabilities.subscribed;
  selection.current = selected;
  outgoingRef.current = outgoing;
  reading.current =
    readingConversation && following && document.visibilityState === "visible";
  const report = useCallback((error: unknown) => {
    const issue = failure(error);
    setConnecting(false);
    if (issue.network) setOnline(false);
    else setError(issue.message);
  }, []);
  const snapshot = snapshots[selected];
  const load = useCallback((id: string) => {
    if (loads.current.has(id)) return loads.current.get(id)!;
    setLoadErrors((prior) => ({ ...prior, [id]: null }));
    const signal = lifetime.current.signal;
    const request = api<Snapshot>(`/api/sessions/${id}`, undefined, {
      signal,
    })
      .then((value) => {
        if (signal.aborted) throw signal.reason;
        if (loads.current.get(id) !== request) return value;
        const accepted = new Set(
          value.state?.view?.blocks?.map((block) => block.request_id),
        );
        setOutgoing((items) =>
          items.filter(
            (item) => item.session_id !== id || !accepted.has(item.request_id),
          ),
        );
        const newer = live.current[id];
        if (newer?.epoch === value.epoch && newer.cursor > value.cursor)
          return newer;
        live.current = retainedViews(
          {
            ...Object.fromEntries(
              Object.entries(live.current).filter(([key]) => key !== id),
            ),
            [id]: value,
          },
          selection.current,
        );
        setSnapshots({ ...live.current });
        return value;
      })
      .catch(async (error) => {
        if (
          failure(error).network &&
          !revoked.current &&
          !signal.aborted &&
          live.current[id]
        )
          return live.current[id];
        if (!signal.aborted && loads.current.get(id) === request)
          setLoadErrors((prior) => ({ ...prior, [id]: error }));
        throw error;
      })
      .finally(() => {
        if (loads.current.get(id) === request) loads.current.delete(id);
      });
    loads.current.set(id, request);
    return request;
  }, []);
  const forget = useCallback((id: string) => {
    loads.current.delete(id);
    delete live.current[id];
    setLoadErrors((prior) => {
      const next = { ...prior };
      delete next[id];
      return next;
    });
    setSnapshots({ ...live.current });
    setDrafts((prior) => {
      const next = { ...prior };
      delete next[id];
      return next;
    });
    setUnread((prior) => {
      const next = new Set(prior);
      next.delete(id);
      return next;
    });
    setCatalogue((prior) => ({
      ...prior,
      sessions: prior.sessions.filter((item) => item.id !== id),
    }));
    if (selection.current === id) {
      selection.current = "";
      writeSelection("", true);
      setSelected("");
    }
  }, []);
  const refresh = useCallback(async () => {
    setManagementVersion((value) => value + 1);
    if (reconnecting.current) {
      refreshAgain.current = true;
      return;
    }
    reconnecting.current = true;
    setConnecting(true);
    stream.current?.close();
    stream.current = undefined;
    loads.current.clear();
    setOnline(false);
    const signal = lifetime.current.signal;
    try {
      const list = await api<Catalogue>("/api/sessions?refresh=1", undefined, {
        signal,
      });
      if (signal.aborted) return;
      revoked.current = false;
      catalogueRef.current = list;
      setCatalogue(list);
      // Activation can precede the first snapshot of a newly created session.
      const knownSessions = new Map(
        list.sessions.map((session) => [session.id, session]),
      );
      setUnread(
        (prior) =>
          new Set([
            ...prior,
            ...list.sessions
              .filter(
                (item) =>
                  (item.incoming || 0) > (readCounts.current[item.id] || 0),
              )
              .map((item) => item.id),
          ]),
      );
      await Promise.all(
        outgoingRef.current.map(async (item) => {
          const receipt = await api<Outcome>(
            `/api/receipts/${item.request_id}`,
            undefined,
            { signal },
          );
          receiveOutcome(receipt);
          setOutgoing((items) =>
            items.map((current) =>
              current.request_id !== item.request_id
                ? current
                : {
                    ...current,
                    status: receipt.unknown
                      ? "Not confirmed"
                      : receipt.pending
                        ? "Awaiting confirmation"
                        : receipt.accepted
                          ? "Sent"
                          : "Not sent",
                    error: receipt.error,
                  },
            ),
          );
        }),
      );
      setAuthenticated(true);
      const id = selection.current;
      if (id && list.sessions.some((item) => item.id === id))
        await load(id).catch(() => {});
      if (signal.aborted) return;
      const events = new EventSource(
        `/api/events?cursor=${list.epoch}:${list.cursor}`,
      );
      stream.current = events;
      events.onopen = () => {
        if (stream.current !== events || signal.aborted) return;
        // Transport-open is not authoritative catch-up. Mutations stay
        // disabled until the server's ready watermark has been applied.
        setConnecting(true);
      };
      events.onerror = () => {
        if (stream.current !== events || signal.aborted) return;
        setOnline(false);
        setConnecting(true);
      };
      events.addEventListener("ready", (message) => {
        if (stream.current !== events || signal.aborted) return;
        let ready: { epoch?: string; cursor?: number };
        try {
          ready = JSON.parse(message.data);
        } catch {
          refresh();
          return;
        }
        if (
          ready.epoch !== list.epoch ||
          typeof ready.cursor !== "number" ||
          ready.cursor < (list.cursor || 0)
        ) {
          refresh();
          return;
        }
        setOnline(true);
        setConnecting(false);
      });
      events.addEventListener("resync", () => {
        if (stream.current === events && !signal.aborted) refresh();
      });
      events.addEventListener("update", (message) => {
        if (stream.current !== events || signal.aborted) return;
        let event: HostEvent;
        try {
          event = JSON.parse(message.data);
          if (
            !event ||
            typeof event.kind !== "string" ||
            typeof event.sequence !== "number" ||
            typeof event.session_id !== "string"
          )
            throw new Error("Invalid event envelope");
        } catch {
          events.close();
          refresh();
          return;
        }
        if (event.v !== protocol || event.epoch !== list.epoch) {
          events.close();
          report(
            new Error(
              "App/server version changed; reconnect or update the app.",
            ),
          );
          setOnline(false);
          return;
        }
        if (
          event.kind === "management.changed" ||
          (event.kind === "event" && event.type === "prompt.changed")
        ) {
          setManagementVersion((version) => version + 1);
          return;
        }
        if (event.kind === "scheduled.changed" && event.scheduled) {
          setCatalogue((prior) => ({ ...prior, scheduled: event.scheduled }));
          return;
        }
        const data = event.data || {};
        if (event.kind === "outcome") {
          receiveOutcome(event);
          setOutgoing((items) =>
            items.map((item) =>
              item.request_id === event.request_id
                ? {
                    ...item,
                    status: event.pending
                      ? "Awaiting confirmation"
                      : event.accepted
                        ? "Sent"
                        : "Not sent",
                    error: event.error,
                  }
                : item,
            ),
          );
        }
        if (event.kind === "gap") {
          refresh();
          return;
        }
        const id = event.session_id;
        const current = live.current[id];
        if (current && event.sequence <= current.cursor) return;
        if (
          current?.metadata.generation &&
          current.metadata.generation !== event.generation &&
          event.kind !== "activated" &&
          event.kind !== "deactivated" &&
          event.kind !== "metadata" &&
          event.kind !== "deleted"
        )
          return;
        if (event.type === "message.changed" && data.block) {
          const block = data.block;
          setOutgoing((items) =>
            items.filter((item) => item.request_id !== block.request_id),
          );
          if (block.incoming)
            setCatalogue((prior) => ({
              ...prior,
              sessions: prior.sessions.map((item) =>
                item.id === id
                  ? {
                      ...item,
                      incoming: Math.max(
                        item.incoming || 0,
                        block.incoming || 0,
                      ),
                    }
                  : item,
              ),
            }));
        }
        if (
          ["activated", "deactivated", "metadata"].includes(event.kind) &&
          event.metadata
        ) {
          knownSessions.set(id, event.metadata!);
          setCatalogue((prior) => ({
            ...prior,
            sessions: [
              event.metadata!,
              ...prior.sessions.filter((item) => item.id !== id),
            ],
          }));
          if (current)
            live.current[id] = {
              ...current,
              epoch: event.epoch,
              cursor: event.sequence,
              metadata: event.metadata!,
              pending: event.kind === "metadata" ? current.pending : null,
            };
        }
        if (event.kind === "deleted") {
          knownSessions.delete(id);
          forget(id);
        }
        if (event.kind === "state") {
          const phase = event.phase || event.state?.phase || "idle";
          const pendingDecision =
            event.pending_decision ?? event.state?.pending_decision ?? null;
          const turnActive = !["idle", "decision"].includes(phase);
          const metadata = {
            ...(current?.metadata || knownSessions.get(id)),
            id,
            generation: event.generation,
            presence: event.presence || "active",
            updated: event.updated ?? current?.metadata.updated,
            status: pendingDecision
              ? "waiting"
              : !event.state?.route
                ? "starting"
                : turnActive
                  ? "running"
                  : "idle",
            turn_active: turnActive,
            pending: !!pendingDecision,
            guidance: event.guidance || 0,
            activity: event.state?.activity,
            activities: event.state?.activities,
            incoming:
              event.state?.statistics?.incoming ||
              current?.metadata.incoming ||
              0,
            title:
              event.state?.title ||
              current?.metadata.title ||
              "New conversation",
          };
          const projected: Snapshot = {
            ...current,
            epoch: event.epoch,
            cursor: event.sequence,
            metadata,
            state: {
              ...event.state,
              phase,
              pending_decision: pendingDecision,
            },
            pending: pendingDecision,
            streamed: event.checkpoint ? [] : current?.streamed,
          };
          live.current[id] = current
            ? mergeCached(current, projected)
            : projected;
          // Decisions and canonical phase changes flush without text batching.
          if (id === selection.current)
            setSnapshots((prior) => ({ ...prior, [id]: live.current[id] }));
          setCatalogue((prior) => ({
            ...prior,
            sessions: prior.sessions.map((item) =>
              item.id === id ? { ...item, ...metadata } : item,
            ),
          }));
        } else if (
          event.kind === "activity" ||
          event.type === "activities.changed" ||
          event.type === "collaborator.changed"
        ) {
          if (current) live.current[id] = applySessionEvent(current, event);
          setCatalogue((prior) => ({
            ...prior,
            sessions: prior.sessions.map((item) =>
              item.id !== id
                ? item
                : {
                    ...item,
                    ...(event.kind === "activity"
                      ? {
                          activity: event.activity,
                          turn_active: event.phase
                            ? !["idle", "decision"].includes(event.phase)
                            : item.turn_active,
                        }
                      : { activities: data.activities }),
                  },
            ),
          }));
        } else if (event.kind === "event" && current) {
          if (
            id === selection.current &&
            event.type === "command.completed" &&
            !!data.request_id &&
            localRequests.current.delete(data.request_id) &&
            (data.output?.trim() || Object.keys(data.result || {}).length)
          ) {
            onResult(
              Object.keys(data.result || {}).length
                ? data.result!
                : data.output!,
              data.inspect !== false,
            );
          }
          if (event.type === "notice" && data.presentation?.status === "failed")
            report(new Error(data.presentation?.title));
          live.current[id] = applySessionEvent(current, event);
        } else if (event.kind === "outcome" && !event.accepted)
          report(new Error(event.error));
        else if (event.kind === "error") report(new Error(event.error));
        else if (event.kind === "closed") {
          if (current)
            live.current[id] = {
              ...current,
              cursor: event.sequence,
              pending: null,
              metadata: {
                ...current.metadata,
                status: "interrupted",
                presence: "",
                turn_active: false,
                activity: "Interrupted",
              },
            };
          setCatalogue((prior) => ({
            ...prior,
            sessions: prior.sessions.map((item) =>
              item.id === id
                ? {
                    ...item,
                    status: "interrupted",
                    presence: "",
                    turn_active: false,
                    activity: "Interrupted",
                  }
                : item,
            ),
          }));
        }
        flush.current(id);
        if (isIncoming(event) && (id !== selection.current || !reading.current))
          setUnread((prior) => new Set([...prior, id]));
        if (isAttention(event)) {
          if (
            notifications.current &&
            !subscribed.current &&
            document.visibilityState !== "visible"
          )
            navigator.serviceWorker?.controller?.postMessage({
              type: "ATTENTION",
              id: `${event.epoch}:${event.sequence}`,
              session_id: id,
            });
        }
      });
    } catch (error) {
      if (signal.aborted) return;
      setConnecting(false);
      if (failure(error).status === 401) {
        revoked.current = true;
        catalogueRef.current = undefined;
        setCatalogue({ sessions: [], devices: [], capabilities: {} });
        setDrafts({});
        setAuthenticated(false);
        live.current = {};
        setSnapshots({});
      } else {
        report(error);
        if (!catalogueRef.current) setAuthenticated((prior) => prior ?? false);
      }
    } finally {
      reconnecting.current = false;
      if (refreshAgain.current) {
        refreshAgain.current = false;
        refresh();
      }
    }
  }, [load, report, forget, onResult]);
  useEffect(() => {
    // Scroll restoration is owned from module scope (see above); the
    // transcript hook is the only writer from the first paint on.
    refresh();
    const recover = (event?: Event) => {
      if (document.visibilityState !== "visible") return;
      if (
        (event instanceof PageTransitionEvent && event.persisted) ||
        !stream.current
      )
        refresh();
      else flush.current();
    };
    const disconnected = () => {
      setConnecting(false);
      stream.current?.close();
      stream.current = undefined;
      setOnline(false);
    };
    let navigation: ReturnType<typeof setTimeout> | undefined;
    const hash = () => {
      clearTimeout(navigation);
      // History traversal restores browser state after popstate. Mount the
      // selected transcript only once that traversal has finished.
      navigation = setTimeout(() => setSelected(selectedFromURL()), 0);
    };
    addEventListener("online", refresh);
    addEventListener("offline", disconnected);
    const suspend = (event: PageTransitionEvent) => {
      if (!event.persisted) return;
      stream.current?.close();
      stream.current = undefined;
      setOnline(false);
    };
    addEventListener("pageshow", recover);
    addEventListener("pagehide", suspend);
    addEventListener("hashchange", hash);
    addEventListener("popstate", hash);
    document.addEventListener("visibilitychange", recover);
    let frame = 0;
    flush.current = (id?: string) => {
      if (id && id !== selection.current) return;
      if (document.visibilityState !== "visible" || frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        live.current = retainedViews(live.current, selection.current);
        setSnapshots({ ...live.current });
      });
    };
    return () => {
      lifetime.current.abort();
      stream.current?.close();
      cancelAnimationFrame(frame);
      clearTimeout(navigation);
      removeEventListener("online", refresh);
      removeEventListener("offline", disconnected);
      removeEventListener("pageshow", recover);
      removeEventListener("pagehide", suspend);
      removeEventListener("hashchange", hash);
      removeEventListener("popstate", hash);
      document.removeEventListener("visibilitychange", recover);
    };
  }, [refresh]);
  useEffect(() => {
    if (selected && authenticated) load(selected).catch(() => {});
  }, [selected, authenticated, load, report]);
  useEffect(() => {
    writeStored(localStorage, "uagent-unread", [...unread]);
  }, [unread]);
  useEffect(() => {
    writeStored(sessionStorage, "uagent-outgoing", outgoing);
  }, [outgoing]);
  useEffect(() => {
    if (
      !readingConversation ||
      !following ||
      document.visibilityState !== "visible" ||
      !snapshot
    )
      return;
    const incoming = snapshot.metadata?.incoming || 0;
    if ((readCounts.current[selected] || 0) < incoming) {
      readCounts.current[selected] = incoming;
      writeStored(localStorage, "uagent-read", readCounts.current);
    }
    setUnread((prior) => {
      if (!prior.has(selected)) return prior;
      const next = new Set(prior);
      next.delete(selected);
      return next;
    });
  }, [selected, following, snapshot, readingConversation]);
  const updateView = useCallback(
    (id: string, update: (snapshot: Snapshot) => Snapshot) => {
      if (!live.current[id]) return;
      live.current[id] = update(live.current[id]);
      setSnapshots({ ...live.current });
    },
    [],
  );
  const correlate = useCallback((requestId: string) => {
    localRequests.current.add(requestId);
    if (localRequests.current.size > 256)
      localRequests.current.delete(
        localRequests.current.values().next().value!,
      );
  }, []);
  function reset() {
    lifetime.current.abort();
    lifetime.current = new AbortController();
    loads.current.clear();
    setLoadErrors({});
    stream.current?.close();
    live.current = {};
    setSnapshots({});
    setDrafts({});
    setOutgoing([]);
    revoked.current = true;
    catalogueRef.current = undefined;
    setCatalogue({ sessions: [], devices: [], capabilities: {} });
    setOnline(false);
    setAuthenticated(false);
    writeSelection("", true);
    setSelected("");
  }
  return {
    managementVersion,
    authenticated,
    online,
    connecting,
    loadErrors,
    catalogue,
    setCatalogue,
    snapshots,
    selected,
    setSelected: (id: string) => {
      selection.current = id;
      writeSelection(id);
      // Following is owned solely by the transcript scroll stick
      // (use-transcript-scroll): a switch resumes the saved position
      // (or pins a fresh surface), which notifies through onFollow.
      // Clearing it here diverged React state (false) from the stick
      // ref (still true), so the pin became a no-op notification
      // and following stayed false forever: the Jump button lingered,
      // unread badges never cleared, and read marking stopped.
      if (live.current[id])
        setSnapshots((prior) =>
          prior[id] === live.current[id]
            ? prior
            : { ...prior, [id]: live.current[id] },
        );
      setSelected(id);
    },
    drafts,
    setDrafts,
    error,
    setError,
    unread,
    outgoing,
    setOutgoing,
    following,
    setFollowing,
    notifications,
    load,
    forget,
    refresh,
    report,
    updateView,
    correlate,
    reset,
  };
}
