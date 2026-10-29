import type { Dispatch, StateUpdater } from "preact/hooks";
import type {
  Catalogue,
  HostEvent,
  JSONValue,
  Outcome,
  Outgoing,
  Session,
  Snapshot,
} from "../shared/types.ts";
import type { SnapshotStore } from "./snapshot-store.ts";
import {
  applySessionEvent,
  confirmOutgoing,
  isIncoming,
  keepOlderPages,
  keptSession,
  raiseIncoming,
  stateFrame,
  withActivities,
} from "./store.ts";

// How each event from the host's stream changes the catalogue, the outgoing
// rows and a session's snapshot. Pure of React so it can be tested alone.

type Ref<T> = { current: T };

// What applying the stream's events reads and changes: the event stream
// owner's state and refs, handed over as they are, so what one event does
// can be followed, and tested, apart from it.
export interface EventHost {
  live: Ref<Record<string, Snapshot>>;
  snapshots: SnapshotStore;
  selection: Ref<string>;
  reading: Ref<boolean>;
  notifications: Ref<boolean>;
  subscribed: Ref<boolean>;
  localRequests: Ref<Set<string>>;
  flush: Ref<(id?: string) => void>;
  setManagementVersion: Dispatch<StateUpdater<number>>;
  setDetail: (level: string) => void;
  setCatalogue: Dispatch<StateUpdater<Catalogue>>;
  patchSession: (id: string, change: (item: Session) => Session) => void;
  upsertSession: (session: Session) => void;
  setOutgoing: Dispatch<StateUpdater<Outgoing[]>>;
  setUnread: Dispatch<StateUpdater<Set<string>>>;
  settleOutgoing: (outcome: Outcome) => boolean;
  forget: (id: string) => void;
  refresh: () => void;
  report: (error: unknown) => void;
  onResult: (value: JSONValue, inspect: boolean) => void;
}

// One event of the stream, applied in order to the catalogue, the outgoing
// rows and its session's snapshot.
export function applyEvent(event: HostEvent, host: EventHost) {
  const {
    live,
    snapshots,
    selection,
    reading,
    notifications,
    subscribed,
    localRequests,
    flush,
    setManagementVersion,
    setDetail,
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
  } = host;
  if (
    event.kind === "management.changed" ||
    (event.kind === "event" && event.type === "prompt.changed")
  ) {
    setManagementVersion((version) => version + 1);
    return;
  }
  // Something saved changed: the detail level, and whatever shows settings.
  if (event.kind === "settings.changed") {
    setDetail(event.level);
    setManagementVersion((version) => version + 1);
    return;
  }
  if (event.kind === "spaces.changed") {
    setCatalogue((prior) => ({ ...prior, spaces: event.spaces }));
    return;
  }
  if (event.kind === "terminals.changed") {
    setCatalogue((prior) => ({ ...prior, terminals: event.terminals }));
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
  // Most state and activity frames restate the session unchanged: it
  // keeps its object, and what shows it skips the frame.
  const activities = () => {
    const metadata = event.metadata;
    if (current)
      live.current[id] = {
        ...applySessionEvent(current, event),
        ...(metadata && {
          metadata: keptSession(current.metadata, metadata),
        }),
      };
    patchSession(id, (item) =>
      metadata
        ? keptSession(item, metadata)
        : withActivities(item, data.activities),
    );
  };
  switch (event.kind) {
    case "node":
      if (event.node) {
        const node = event.node;
        setOutgoing((items) => confirmOutgoing(items, node.request_id));
        const incoming = node.incoming;
        if (incoming) patchSession(id, (item) => raiseIncoming(item, incoming));
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
        metadata: keptSession(current?.metadata, metadata),
        state: stateFrame(current?.state, event.state, phase),
        pending: pendingDecision,
      };
      live.current[id] = keepOlderPages(current, projected);
      // Decisions and canonical phase changes flush without text batching.
      if (id === selection.current || snapshots.watches(id))
        snapshots.set((prior) => ({ ...prior, [id]: live.current[id] }));
      patchSession(id, (item) => keptSession(item, metadata));
      break;
    }
    case "turn_status":
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
}
