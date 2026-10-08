import { storage } from "../shared/storage.ts";
import type {
  JSONValue,
  Snapshot,
  Catalogue,
  Draft,
  Outgoing,
  HostEvent,
  Outcome,
  Session,
} from "../shared/types.ts";
import { failure } from "../shared/types.ts";
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "preact/hooks";
import {
  retainedViews,
  applySessionEvent,
  isIncoming,
  keepOlderPages,
  stateFrame,
  readStored,
  writeStored,
  hasContent,
  patchCatalogue,
  withActivities,
  raiseIncoming,
  confirmOutgoing,
} from "./store.ts";
import { api, manage, protocol, receiveOutcome } from "./api.ts";
import { snapshotStore } from "./snapshot-store.ts";
import { queuedGuidance } from "../shared/message-view.ts";
import { selectedFromURL, writeSelection } from "../shared/navigation.ts";
import {
  maxLocalRequests,
  reconnectGraceMs,
  reconnectMaxDelayMs,
} from "../shared/limits.ts";
import type { ConnectionPhase } from "../shared/connection-status.tsx";
import { needsYou } from "./attention.ts";

const CATALOGUE_KEY = "uagent-catalogue";

// What the sidebar lists: conversations, not scheduled runs or their tasks.
function listedSessions({
  sessions,
  scheduled,
}: Pick<Catalogue, "sessions" | "scheduled">) {
  const runs = new Set(scheduled?.runs?.map((run) => run.session_id));
  return sessions.filter((item) => !item.task_id && !runs.has(item.id));
}

// A stream message's event, or nothing when its envelope is not one.
function envelope(data: string): HostEvent | undefined {
  try {
    const event = JSON.parse(data);
    if (
      event &&
      typeof event.kind === "string" &&
      typeof event.sequence === "number" &&
      typeof event.session_id === "string"
    )
      return event;
  } catch {
    /* Not JSON. */
  }
  return undefined;
}

// One SSE subscription owns host snapshots, command receipts and read state.
export function useHost(
  onResult: (value: JSONValue, inspect: boolean) => void,
  readingConversation = true,
) {
  const [managementVersion, setManagementVersion] = useState(0);
  const [authenticated, setAuthenticated] = useState<boolean | null>(null);
  const [online, setOnline] = useState(false);
  const [connecting, setConnecting] = useState(true);
  const connectedOnce = useRef(false);
  // Resuming usually catches up within a second; only a slower reconnect is
  // worth a label. Mutations stay gated on `online` either way.
  const [slowReconnect, setSlowReconnect] = useState(false);
  const [loadErrors, setLoadErrors] = useState<Record<string, unknown>>({});
  const revoked = useRef(false);
  const catalogueRef = useRef<Catalogue>();
  const loads = useRef(new Map<string, Promise<Snapshot>>());
  const lifetime = useRef(new AbortController());
  // The last list this device saw paints the sidebar, title and composer
  // from the first frame of a reload; the host's answer replaces it, and
  // everything stays inert until then (`online` is false).
  const [catalogue, setCatalogue] = useState<Catalogue>(() => {
    const sessions = readStored<Catalogue["sessions"]>(
      storage,
      CATALOGUE_KEY,
      [],
    );
    return { sessions, capabilities: {}, devices: [] };
  });
  const [snapshots] = useState(snapshotStore);
  const [selected, setSelected] = useState(selectedFromURL);
  // Unsent drafts survive an evicted or reloaded app; logout clears them.
  // An upload still in flight when the page went away never finishes; its
  // chip would have no id the host knows.
  const [drafts, setDrafts] = useState(() =>
    Object.fromEntries(
      Object.entries(
        readStored<Record<string, Draft>>(storage, "uagent-drafts", {}),
      ).map(([id, draft]) => [
        id,
        { ...draft, files: draft.files.filter((file) => !file.pending) },
      ]),
    ),
  );
  const [error, setError] = useState("");
  const [unread, setUnread] = useState(
    () => new Set(readStored<string[]>(storage, "uagent-unread", [])),
  );
  const [outgoing, setOutgoing] = useState(() =>
    readStored<Outgoing[]>(sessionStorage, "uagent-outgoing", []),
  );
  const [following, setFollowing] = useState(true);
  const stream = useRef<EventSource>();
  const reconnecting = useRef(false);
  const refreshAgain = useRef(false);
  // A failed load retries on its own, backing off to reconnectMaxDelayMs.
  const retry = useRef<{
    attempt: number;
    timer?: ReturnType<typeof setTimeout>;
  }>({ attempt: 0 });
  const live = useRef<Record<string, Snapshot>>({});
  const selection = useRef(selected);
  const flush = useRef((_id?: string) => {});
  const notifications = useRef(false);
  const subscribed = useRef(false);
  const reading = useRef(false);
  const readCounts = useRef(
    readStored<Record<string, number>>(storage, "uagent-read", {}),
  );
  const outgoingRef = useRef(outgoing);
  const localRequests = useRef(new Set<string>());
  subscribed.current = !!catalogue.capabilities.subscribed;
  selection.current = selected;
  outgoingRef.current = outgoing;
  reading.current =
    readingConversation && following && document.visibilityState === "visible";
  // A failed request means offline only when the event stream agrees; with
  // the stream open it was one request, reported like any other failure.
  // An inline report is shown where it happened (a message, a decision);
  // it only tells whether the host is gone.
  const report = useCallback((error: unknown, scope?: "inline") => {
    const issue = failure(error);
    if (issue.network && stream.current?.readyState !== EventSource.OPEN)
      setOnline(false);
    else if (scope !== "inline") setError(issue.message);
  }, []);
  const patchSession = (id: string, change: (item: Session) => Session) =>
    setCatalogue((prior) => patchCatalogue(prior, id, change));
  // The global verbosity level, as the host last stated it.
  const setVerbosityLevel = (level: string) =>
    setCatalogue((prior) =>
      prior.verbosity && prior.verbosity.level !== level
        ? { ...prior, verbosity: { ...prior.verbosity, level } }
        : prior,
    );
  // Changing it is a setting of the host's: every browser hears of it, and
  // this one need not wait to.
  const setVerbosity = useCallback(async (level: string) => {
    const saved = await manage("config", {
      scope: "user",
      operation: "apply",
      changes: [{ key: "UAGENT_VERBOSITY", value: level }],
    });
    // Saved, yet the environment or a project's config still decides it.
    const shadowed = saved.effects.find((item) => item.effect === "shadowed");
    if (shadowed) throw new Error(`${shadowed.key}: ${shadowed.text}`);
    setVerbosityLevel(level);
  }, []);
  // A new or reactivated session leads the list.
  const upsertSession = useCallback(
    (session: Session) =>
      setCatalogue((prior) => ({
        ...prior,
        sessions: [
          session,
          ...prior.sessions.filter((item) => item.id !== session.id),
        ],
      })),
    [],
  );
  const patchOutgoing = useCallback(
    (
      requestId: string,
      patch: Partial<Outgoing> | ((item: Outgoing) => Partial<Outgoing>),
    ) =>
      setOutgoing((items) =>
        items.map((item) =>
          item.request_id !== requestId
            ? item
            : {
                ...item,
                ...(typeof patch === "function" ? patch(item) : patch),
              },
        ),
      ),
    [],
  );
  const dropStream = () => {
    stream.current?.close();
    stream.current = undefined;
    setOnline(false);
  };
  // What this device drops when the host no longer knows it.
  const signedOut = () => {
    revoked.current = true;
    catalogueRef.current = undefined;
    setCatalogue({ sessions: [], devices: [], capabilities: {} });
    storage.removeItem(CATALOGUE_KEY);
    setDrafts({});
    setAuthenticated(false);
    live.current = {};
    snapshots.set({});
  };
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
        snapshots.set({ ...live.current });
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
    delete readCounts.current[id];
    setLoadErrors((prior) => {
      const next = { ...prior };
      delete next[id];
      return next;
    });
    snapshots.set({ ...live.current });
    setDrafts((prior) => {
      const next = { ...prior };
      delete next[id];
      return next;
    });
    // A queued message for a deleted session can never send (load() has
    // no session to reconcile it against): drop it with the session
    // instead of orphaning it in storage.
    setOutgoing((prior) => prior.filter((item) => item.session_id !== id));
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
  // A command's outcome, from its receipt or the stream, settles the
  // outgoing row that sent it. True when a row or a waiting caller shows
  // it, so a refusal needs no banner.
  const settleOutgoing = (outcome: Outcome) => {
    const awaited = receiveOutcome(outcome);
    patchOutgoing(outcome.request_id, (item) => ({
      status: outcome.unknown
        ? "Not confirmed"
        : outcome.pending
          ? "Awaiting confirmation"
          : outcome.accepted
            ? // Accepted guidance waits for its step and stays
              // recallable until the transcript shows it.
              queuedGuidance(item)
              ? item.status
              : "Sent"
            : "Not sent",
      error: outcome.error,
    }));
    return (
      awaited ||
      outgoingRef.current.some((item) => item.request_id === outcome.request_id)
    );
  };
  // Jittered exponential backoff; a successful catalogue read resets it.
  const retryLater = () => {
    const delay = Math.min(
      reconnectMaxDelayMs,
      1000 * 2 ** retry.current.attempt++,
    );
    retry.current.timer = setTimeout(
      () => refresh(),
      delay * (0.5 + Math.random() / 2),
    );
  };
  // One event of the stream, applied in order to the catalogue, the outgoing
  // rows and its session's snapshot.
  const applyEvent = (event: HostEvent) => {
    if (
      event.kind === "management.changed" ||
      (event.kind === "event" && event.type === "prompt.changed")
    ) {
      setManagementVersion((version) => version + 1);
      return;
    }
    // Something saved changed: the detail level, and whatever shows settings.
    if (event.kind === "settings.changed") {
      setVerbosityLevel(event.level);
      setManagementVersion((version) => version + 1);
      return;
    }
    if (event.kind === "scheduled.changed" && event.scheduled) {
      setCatalogue((prior) => ({ ...prior, scheduled: event.scheduled }));
      return;
    }
    const data = event.data || {};
    const shown = event.kind === "outcome" && settleOutgoing(event);
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
    const activities = () => {
      if (current)
        live.current[id] = {
          ...applySessionEvent(current, event),
          ...(event.metadata && { metadata: event.metadata }),
        };
      patchSession(
        id,
        (item) => event.metadata || withActivities(item, data.activities),
      );
    };
    switch (event.kind) {
      case "block":
        if (event.block) {
          const block = event.block;
          setOutgoing((items) => confirmOutgoing(items, block.request_id));
          if (block.incoming)
            patchSession(id, (item) => raiseIncoming(item, block.incoming!));
        }
        if (current) live.current[id] = applySessionEvent(current, event);
        break;
      case "activated":
      case "deactivated":
      case "metadata":
        if (!event.metadata) break;
        upsertSession(event.metadata);
        if (current)
          live.current[id] = {
            ...current,
            epoch: event.epoch,
            cursor: event.sequence,
            metadata: event.metadata,
            pending: event.kind === "metadata" ? current.pending : null,
          };
        break;
      case "deleted":
        forget(id);
        break;
      case "state": {
        const phase = event.phase || event.state?.phase || "idle";
        const pendingDecision = event.pending ?? null;
        const metadata = event.metadata!;
        const projected: Snapshot = {
          ...current,
          epoch: event.epoch,
          cursor: event.sequence,
          metadata,
          state: stateFrame(current?.state, event.state, phase),
          pending: pendingDecision,
        };
        live.current[id] = keepOlderPages(current, projected);
        // Decisions and canonical phase changes flush without text batching.
        if (id === selection.current)
          snapshots.set((prior) => ({ ...prior, [id]: live.current[id] }));
        patchSession(id, () => metadata);
        break;
      }
      case "activity":
        activities();
        break;
      case "event":
        if (event.type === "activities.changed") {
          activities();
          break;
        }
        if (!current) break;
        if (
          id === selection.current &&
          event.type === "command.completed" &&
          !!data.request_id &&
          localRequests.current.delete(data.request_id) &&
          (data.output?.trim() || Object.keys(data.result || {}).length)
        ) {
          onResult(
            Object.keys(data.result || {}).length ? data.result! : data.output!,
            data.inspect !== false,
          );
        }
        if (event.type === "notice" && data.presentation?.status === "failed")
          report(new Error(data.presentation?.title));
        live.current[id] = applySessionEvent(current, event);
        break;
      case "outcome":
        if (!event.accepted && !shown) report(new Error(event.error));
        break;
      case "error":
        report(new Error(event.error));
        break;
      case "closed": {
        const metadata = event.metadata;
        if (!metadata) break;
        if (current)
          live.current[id] = {
            ...current,
            cursor: event.sequence,
            pending: null,
            metadata,
          };
        patchSession(id, () => metadata);
        break;
      }
    }
    flush.current(id);
    if (isIncoming(event) && (id !== selection.current || !reading.current))
      setUnread((prior) => (prior.has(id) ? prior : new Set([...prior, id])));
    // The host decides what needs a person; its id dedupes with push.
    if (event.attention_id) {
      if (
        notifications.current &&
        !subscribed.current &&
        document.visibilityState !== "visible"
      )
        navigator.serviceWorker?.controller?.postMessage({
          type: "ATTENTION",
          id: event.attention_id,
          session_id: id,
        });
    }
  };
  const refresh = useCallback(async () => {
    clearTimeout(retry.current.timer);
    setManagementVersion((value) => value + 1);
    if (reconnecting.current) {
      refreshAgain.current = true;
      return;
    }
    reconnecting.current = true;
    setConnecting(true);
    dropStream();
    loads.current.clear();
    const signal = lifetime.current.signal;
    try {
      const list = await api<Catalogue>("/api/sessions?refresh=1", undefined, {
        signal,
      });
      if (signal.aborted) return;
      retry.current.attempt = 0;
      revoked.current = false;
      catalogueRef.current = list;
      setCatalogue(list);
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
          settleOutgoing({ ...receipt, request_id: item.request_id });
        }),
      );
      // Catalogue access establishes authentication. Conversation data can
      // fail or stall independently; expose the shell so another session and
      // the retry action remain reachable while its snapshot loads.
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
        setConnecting(events.readyState === EventSource.CONNECTING);
        // A non-200 answer (a proxy's 502 mid-restart) closes the stream
        // for good; only a fresh refresh reconnects.
        if (events.readyState === EventSource.CLOSED) retryLater();
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
        connectedOnce.current = true;
        setOnline(true);
        setConnecting(false);
      });
      events.addEventListener("resync", () => {
        if (stream.current === events && !signal.aborted) refresh();
      });
      events.addEventListener("update", (message) => {
        if (stream.current !== events || signal.aborted) return;
        const event = envelope(message.data);
        if (!event) {
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
          setConnecting(false);
          return;
        }
        applyEvent(event);
      });
    } catch (error) {
      if (signal.aborted) return;
      setConnecting(false);
      if (failure(error).status === 401) signedOut();
      else {
        report(error);
        if (!catalogueRef.current) setAuthenticated((prior) => prior ?? false);
        retryLater();
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
      dropStream();
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
    const suspend = (event: PageTransitionEvent) =>
      event.persisted && disconnected();
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
        snapshots.set({ ...live.current });
      });
    };
    return () => {
      clearTimeout(retry.current.timer);
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
  }, [selected, authenticated, load]);
  // Only what the sidebar lists: scheduled runs and their tasks stay out.
  const listed = useMemo(
    () => listedSessions(catalogue),
    [catalogue.sessions, catalogue.scheduled],
  );
  useEffect(() => {
    if (authenticated) writeStored(storage, CATALOGUE_KEY, listed);
  }, [authenticated, listed]);
  // The tab title and the installed app's badge count what needs you;
  // written only when that count changes.
  const attention = needsYou(listed);
  useEffect(() => {
    document.title = attention ? `(${attention}) µAgent` : "µAgent";
    if ("setAppBadge" in navigator)
      (attention
        ? navigator.setAppBadge(attention)
        : navigator.clearAppBadge()
      ).catch(() => {});
  }, [attention]);
  useEffect(() => {
    writeStored(storage, "uagent-unread", [...unread]);
  }, [unread]);
  useEffect(() => {
    writeStored(sessionStorage, "uagent-outgoing", outgoing);
  }, [outgoing]);
  useEffect(() => {
    writeStored(
      storage,
      "uagent-drafts",
      Object.fromEntries(
        Object.entries(drafts).filter(([, draft]) => hasContent(draft)),
      ),
    );
  }, [drafts]);
  // Reading the conversation marks each of its snapshots read.
  useEffect(() => {
    if (!readingConversation || !following) return;
    const read = () => {
      const snapshot = snapshots.get()[selected];
      if (document.visibilityState !== "visible" || !snapshot) return;
      const incoming = snapshot.metadata?.incoming || 0;
      if ((readCounts.current[selected] || 0) < incoming) {
        readCounts.current[selected] = incoming;
        writeStored(storage, "uagent-read", readCounts.current);
      }
      setUnread((prior) => {
        if (!prior.has(selected)) return prior;
        const next = new Set(prior);
        next.delete(selected);
        return next;
      });
    };
    read();
    return snapshots.subscribe(read);
  }, [selected, following, readingConversation, snapshots]);
  const updateView = useCallback(
    (id: string, update: (snapshot: Snapshot) => Snapshot) => {
      if (!live.current[id]) return;
      live.current[id] = update(live.current[id]);
      snapshots.set({ ...live.current });
    },
    [],
  );
  const select = useCallback((id: string) => {
    selection.current = id;
    writeSelection(id);
    // Following is owned solely by the transcript history controller:
    // a switch resumes the saved position
    // (or pins a fresh surface), which notifies through onFollow.
    // Clearing it here diverged React state (false) from the stick
    // ref (still true), so the pin became a no-op notification
    // and following stayed false forever: the Jump button lingered,
    // unread badges never cleared, and read marking stopped.
    if (live.current[id])
      snapshots.set((prior) =>
        prior[id] === live.current[id]
          ? prior
          : { ...prior, [id]: live.current[id] },
      );
    setSelected(id);
  }, []);
  const correlate = useCallback((requestId: string) => {
    localRequests.current.add(requestId);
    if (localRequests.current.size > maxLocalRequests)
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
    setOutgoing([]);
    signedOut();
    setOnline(false);
    setConnecting(false);
    connectedOnce.current = false;
    writeSelection("", true);
    setSelected("");
  }
  useEffect(() => {
    setSlowReconnect(false);
    if (online) return;
    const timer = setTimeout(() => setSlowReconnect(true), reconnectGraceMs);
    return () => clearTimeout(timer);
  }, [online]);
  return {
    managementVersion,
    authenticated,
    online,
    connection: (online
      ? "connected"
      : connecting
        ? connectedOnce.current
          ? slowReconnect
            ? "reconnecting"
            : "connected"
          : "connecting"
        : "disconnected") as ConnectionPhase,
    loadErrors,
    catalogue,
    listed,
    upsertSession,
    setVerbosity,
    snapshots,
    selected,
    setSelected: select,
    drafts,
    setDrafts,
    error,
    setError,
    unread,
    outgoing,
    setOutgoing,
    patchOutgoing,
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
