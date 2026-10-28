import { stored, tabStorage, useStored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import type {
  JSONValue,
  Snapshot,
  Catalogue,
  ConnectionPhase,
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
  adoptSnapshot,
  retainedViews,
  hasContent,
  patchCatalogue,
} from "./store.ts";
import { applyEvent as applyHostEvent } from "./apply-event.ts";
import { api, manage, protocol, receiveOutcome, routes } from "./api.ts";
import { snapshotStore } from "./snapshot-store.ts";
import { outgoingStatus, queuedGuidance } from "../shared/message-view.ts";
import { selectedFromURL, writeSelection } from "../shared/navigation.ts";
import {
  catalogueSaveDelayMs,
  draftSaveDelayMs,
  maxLocalRequests,
  reconnectBaseDelayMs,
  reconnectGraceMs,
  reconnectMaxDelayMs,
} from "../shared/limits.ts";
import { needsYou } from "./attention.ts";

// What this device keeps of the host's state. A draft is saved once typing
// pauses, not per key; an upload still in flight when the page went away
// never finishes, and its chip would have no id the host knows.
const kept = {
  // Streamed events change the list many times a second; it is kept once
  // they pause, and when the page hides.
  catalogue: stored<Catalogue["sessions"]>(
    storageKeys.catalogue,
    [],
    undefined,
    {
      delay: catalogueSaveDelayMs,
    },
  ),
  spaces: stored<NonNullable<Catalogue["spaces"]>>(storageKeys.spaces, []),
  drafts: stored<Record<string, Draft>>(
    storageKeys.drafts,
    {},
    (value) =>
      Object.fromEntries(
        Object.entries(
          (value && typeof value === "object" ? value : {}) as Record<
            string,
            Draft
          >,
        ).map(([id, draft]) => [
          id,
          { ...draft, files: draft.files.filter((file) => !file.pending) },
        ]),
      ),
    {
      delay: draftSaveDelayMs,
      kept: (value) =>
        Object.fromEntries(
          Object.entries(value).filter(([, draft]) => hasContent(draft)),
        ),
    },
  ),
  unread: stored(
    storageKeys.unread,
    new Set<string>(),
    (value) => new Set(Array.isArray(value) ? (value as string[]) : []),
    { kept: (value) => [...value] },
  ),
  outgoing: stored<Outgoing[]>(storageKeys.outgoing, [], undefined, {
    where: tabStorage,
  }),
  read: stored<Record<string, number>>(storageKeys.read, {}),
};

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

export type HostState = ReturnType<typeof useHost>;

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
  const catalogueRef = useRef<Catalogue | undefined>(undefined);
  const loads = useRef(new Map<string, Promise<Snapshot>>());
  const lifetime = useRef(new AbortController());
  // The last list this device saw paints the sidebar, title and composer
  // from the first frame of a reload; the host's answer replaces it, and
  // everything stays inert until then (`online` is false).
  const [catalogue, setCatalogue] = useState<Catalogue>(() => ({
    sessions: kept.catalogue.read(),
    spaces: kept.spaces.read(),
    capabilities: {},
    devices: [],
  }));
  const [snapshots] = useState(snapshotStore);
  const [selected, setSelected] = useState(selectedFromURL);
  // Unsent drafts survive an evicted or reloaded app; logout clears them.
  const [drafts, setDrafts] = useStored(kept.drafts);
  const [error, setError] = useState("");
  const [unread, setUnread] = useStored(kept.unread);
  const [outgoing, setOutgoing] = useStored(kept.outgoing);
  const [following, setFollowing] = useState(true);
  const stream = useRef<EventSource | undefined>(undefined);
  const reconnecting = useRef(false);
  const refreshAgain = useRef(false);
  // Whether the next catalogue read has the host rescan its sessions on
  // disk: at start, when the stream lost its cursor, or when asked. A
  // reconnect reads the catalogue the host keeps, whose changes still
  // follow on the stream.
  const rescan = useRef(false);
  const resuming = useRef(false);
  // epoch:cursor of the last event applied, where a reconnect picks up.
  const resume = useRef("");
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
  const readCounts = useRef(kept.read.read());
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
  const setVerbosityLevel = useCallback(
    (level: string) =>
      setCatalogue((prior) =>
        prior.verbosity && prior.verbosity.level !== level
          ? { ...prior, verbosity: { ...prior.verbosity, level } }
          : prior,
      ),
    [],
  );
  // Changing it is a setting of the host's: every browser hears of it, and
  // this one need not wait to.
  const setVerbosity = useCallback(
    async (level: string) => {
      const saved = await manage("config", {
        scope: "user",
        operation: "apply",
        changes: [{ key: "UAGENT_VERBOSITY", value: level }],
      });
      // Saved, yet the environment or a folder's config still decides it.
      const shadowed = saved.effects.find((item) => item.effect === "shadowed");
      if (shadowed) throw new Error(`${shadowed.key}: ${shadowed.text}`);
      setVerbosityLevel(level);
    },
    [setVerbosityLevel],
  );
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
  const dropStream = useCallback(() => {
    stream.current?.close();
    stream.current = undefined;
    setOnline(false);
  }, []);
  // What this device drops when the host no longer knows it.
  const signedOut = useCallback(() => {
    revoked.current = true;
    catalogueRef.current = undefined;
    setCatalogue({ sessions: [], devices: [], capabilities: {} });
    kept.catalogue.write([]);
    kept.spaces.write([]);
    setDrafts({});
    setAuthenticated(false);
    live.current = {};
    snapshots.set({});
  }, [snapshots]);
  const load = useCallback(
    (id: string) => {
      if (loads.current.has(id)) return loads.current.get(id)!;
      setLoadErrors((prior) => ({ ...prior, [id]: null }));
      const signal = lifetime.current.signal;
      const request = api<Snapshot>(routes.session(id), undefined, {
        signal,
      })
        .then(adoptSnapshot)
        .then((value) => {
          if (signal.aborted) throw signal.reason;
          if (loads.current.get(id) !== request) return value;
          const accepted = new Set(
            value.state?.view?.blocks?.map((block) =>
              "request_id" in block ? block.request_id : undefined,
            ),
          );
          setOutgoing((items) =>
            items.filter(
              (item) =>
                item.session_id !== id || !accepted.has(item.request_id),
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
            snapshots.watching(),
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
    },
    [snapshots],
  );
  const forget = useCallback(
    (id: string) => {
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
    },
    [snapshots],
  );
  // A command's outcome, from its receipt or the stream, settles the
  // outgoing row that sent it. True when a row or a waiting caller shows
  // it, so a refusal needs no banner.
  const settleOutgoing = useCallback(
    (outcome: Outcome) => {
      const awaited = receiveOutcome(outcome);
      patchOutgoing(outcome.request_id, (item) => ({
        status: outcome.unknown
          ? outgoingStatus.unconfirmed
          : outcome.pending
            ? queuedGuidance(item)
              ? item.status
              : outgoingStatus.awaiting
            : outcome.accepted
              ? // Accepted guidance waits for its step and stays
                // recallable until the transcript shows it.
                queuedGuidance(item)
                ? item.status
                : outgoingStatus.sent
              : outgoingStatus.refused,
        error: outcome.error,
      }));
      return (
        awaited ||
        outgoingRef.current.some(
          (item) => item.request_id === outcome.request_id,
        )
      );
    },
    [patchOutgoing],
  );
  // Jittered exponential backoff; a successful catalogue read resets it.
  const retryLater = () => {
    const delay = Math.min(
      reconnectMaxDelayMs,
      reconnectBaseDelayMs * 2 ** retry.current.attempt++,
    );
    retry.current.timer = setTimeout(
      () => reconnect(),
      delay * (0.5 + Math.random() / 2),
    );
  };
  const applyEvent = (event: HostEvent) =>
    applyHostEvent(event, {
      live,
      snapshots,
      selection,
      reading,
      notifications,
      subscribed,
      localRequests,
      flush,
      setManagementVersion,
      setVerbosityLevel,
      setCatalogue,
      patchSession,
      upsertSession,
      setOutgoing,
      setUnread,
      settleOutgoing,
      forget,
      refresh,
      report,
      onResult,
    });
  // retryLater and applyEvent call refresh again, so they cannot be listed;
  // what they capture (report, forget, onResult, ...) is.
  // biome-ignore lint/correctness/useExhaustiveDependencies: see above
  const refresh = useCallback(async () => {
    rescan.current ||= !resuming.current;
    resuming.current = false;
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
    // Opens the event stream after `from` (epoch:cursor); the host replays
    // what came after it, or answers resync when it can no longer.
    const follow = (list: Catalogue, from: string) => {
      const floor = Number(from.slice(from.indexOf(":") + 1));
      resume.current = from;
      const events = new EventSource(routes.events(from));
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
        // for good; only a fresh refresh reconnects, and one that cannot
        // resume, as the catalogue read is what learns a 401.
        if (events.readyState === EventSource.CLOSED) {
          resume.current = "";
          retryLater();
        }
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
          ready.cursor < floor
        ) {
          refresh();
          return;
        }
        retry.current.attempt = 0;
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
        resume.current = message.lastEventId || resume.current;
      });
    };
    // A reconnect of a view this device holds reopens the stream at the last
    // event it applied, and keeps the catalogue and snapshots it has: only a
    // start, a rescan or a lost cursor reads them again.
    const known = catalogueRef.current;
    if (!rescan.current && resume.current && known) {
      try {
        follow(known, resume.current);
      } finally {
        reconnecting.current = false;
        if (refreshAgain.current) {
          refreshAgain.current = false;
          reconnect();
        }
      }
      return;
    }
    // The open conversation is asked for with the list, not after it, when
    // this device listed it before; the list's own load below reuses it.
    const open = selection.current;
    if (
      open &&
      (catalogueRef.current?.sessions ?? kept.catalogue.read()).some(
        (item) => item.id === open,
      )
    )
      load(open).catch(() => {});
    try {
      const scan = rescan.current;
      rescan.current = false;
      const list = await api<Catalogue>(
        scan ? routes.refreshedSessions : routes.sessions,
        undefined,
        { signal },
      );
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
            routes.receipt(item.request_id),
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
      follow(list, `${list.epoch}:${list.cursor}`);
    } catch (error) {
      if (signal.aborted) return;
      setConnecting(false);
      // Only the host's refusal asks for pairing: a host that cannot be
      // reached keeps the app on screen while it is tried again.
      if (failure(error).status === 401) signedOut();
      else {
        report(error);
        retryLater();
      }
    } finally {
      reconnecting.current = false;
      if (refreshAgain.current) {
        refreshAgain.current = false;
        reconnect();
      }
    }
  }, [load, report, forget, onResult, dropStream, signedOut, settleOutgoing]);
  // Reads the catalogue again without asking for a rescan; one a refresh
  // asked for meanwhile still happens.
  const reconnect = useCallback(() => {
    resuming.current = true;
    refresh();
  }, [refresh]);
  useEffect(() => {
    refresh();
    const recover = (event?: Event) => {
      if (document.visibilityState !== "visible") return;
      if (
        (event instanceof PageTransitionEvent && event.persisted) ||
        !stream.current
      )
        reconnect();
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
    addEventListener("online", reconnect);
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
      if (id && id !== selection.current && !snapshots.watches(id)) return;
      if (document.visibilityState !== "visible" || frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        live.current = retainedViews(
          live.current,
          selection.current,
          snapshots.watching(),
        );
        snapshots.set({ ...live.current });
      });
    };
    return () => {
      clearTimeout(retry.current.timer);
      lifetime.current.abort();
      stream.current?.close();
      cancelAnimationFrame(frame);
      clearTimeout(navigation);
      removeEventListener("online", reconnect);
      removeEventListener("offline", disconnected);
      removeEventListener("pageshow", recover);
      removeEventListener("pagehide", suspend);
      removeEventListener("hashchange", hash);
      removeEventListener("popstate", hash);
      document.removeEventListener("visibilitychange", recover);
    };
  }, [refresh, reconnect, dropStream, snapshots]);
  useEffect(() => {
    if (selected && authenticated) load(selected).catch(() => {});
  }, [selected, authenticated, load]);
  // Only what the sidebar lists: scheduled runs and their tasks stay out.
  const listed = useMemo(
    () =>
      listedSessions({
        sessions: catalogue.sessions,
        scheduled: catalogue.scheduled,
      }),
    [catalogue.sessions, catalogue.scheduled],
  );
  useEffect(() => {
    if (authenticated) kept.catalogue.write(listed);
  }, [authenticated, listed]);
  useEffect(() => {
    if (authenticated && catalogue.spaces) kept.spaces.write(catalogue.spaces);
  }, [authenticated, catalogue.spaces]);
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
  // Reading the conversation marks each of its snapshots read.
  useEffect(() => {
    if (!readingConversation || !following) return;
    const read = () => {
      const snapshot = snapshots.get()[selected];
      if (document.visibilityState !== "visible" || !snapshot) return;
      const incoming = snapshot.metadata?.incoming || 0;
      if ((readCounts.current[selected] || 0) < incoming) {
        readCounts.current[selected] = incoming;
        kept.read.write(readCounts.current);
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
    [snapshots],
  );
  const select = useCallback(
    (id: string) => {
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
    },
    [snapshots],
  );
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
    attention,
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
