import "../../shared/activity.css";
import { duration } from "../../shared/duration.ts";
import { useElapsed } from "../../shared/time.ts";
import type {
  Activity,
  ActivityDetail,
  InspectorTarget,
  Report,
  SessionRef,
} from "../../shared/types.ts";
import { useEffect, useRef, useState } from "preact/hooks";
import { command, manage } from "../../state/api.ts";
import {
  cleanText,
  EmptyState,
  LoadError,
  Modal,
  Skeleton,
  Spinner,
} from "../../shared/ui.tsx";
import { active, ActivityControls } from "../../shared/turn-status.tsx";
import Raw from "./raw.tsx";
import Markdown from "../../shared/markdown-view.tsx";

// Pure fetch half of an activity inspect: memory content or the
// activity's output. State (version guards, loading, errors) stays with the
// caller.
async function fetchActivityDetail(
  session: SessionRef,
  cwd: string,
  item: ActivityDetail,
  signal?: AbortSignal,
): Promise<ActivityDetail> {
  if (item.memory) {
    const result = await manage(
      "memory",
      {
        action: "get",
        cwd,
        key: item.memory.key,
      },
      { signal },
    );
    if (!result.item || result.item.error)
      throw new Error(result.item?.error || "Memory is no longer available.");
    return { ...item, output: result.item.content || "" };
  }
  const response = await command(
    "activity",
    session,
    {
      operation: "inspect",
      activity_id: item.id || item.activity_id || 0,
    },
    { signal },
  );
  if (response.pending)
    throw new Error("Activity is still loading. Try again shortly.");
  return { ...item, ...response.result };
}

// One inspector for what the agent started: a command, a memory or a
// tool's input/output, in a side sheet.
export default function Inspector({
  target,
  items,
  cwd,
  session,
  online,
  report,
  close,
}: {
  target: InspectorTarget;
  items: Activity[];
  cwd: string;
  session: SessionRef;
  online: boolean;
  report: Report;
  close: () => void;
}) {
  const [detail, setDetail] = useState<ActivityDetail | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const inspection = useRef(0);
  const inspectionRequest = useRef<AbortController | undefined>(undefined);
  // Progress ticks can outpace a refresh: one runs at a time and the latest
  // tick waits for it, instead of each tick aborting the one before.
  const refreshing = useRef(false);
  const queuedRefresh = useRef<ActivityDetail | null>(null);

  useEffect(
    () => () => {
      ++inspection.current;
      inspectionRequest.current?.abort();
    },
    [],
  );

  async function inspect(item: ActivityDetail, refresh = false) {
    if (refresh && refreshing.current) {
      queuedRefresh.current = item;
      return;
    }
    refreshing.current = refresh;
    const version = ++inspection.current;
    inspectionRequest.current?.abort();
    const controller = new AbortController();
    inspectionRequest.current = controller;
    setDetail((prior) => (prior?.id === item.id ? prior : item));
    if (!refresh) {
      setLoading(true);
      setError(null);
    }
    try {
      const next = await fetchActivityDetail(
        session,
        cwd,
        item,
        controller.signal,
      );
      if (version !== inspection.current) return;
      setDetail(next);
    } catch (failure) {
      if (version === inspection.current && !refresh) setError(failure);
    } finally {
      if (version === inspection.current && !refresh) setLoading(false);
      if (refresh) {
        refreshing.current = false;
        const queued = queuedRefresh.current;
        queuedRefresh.current = null;
        if (queued && version === inspection.current)
          inspect(queued, true).catch(report);
      }
    }
  }

  // A transcript row carries the command it ran (bounded 8k) and its text, so
  // an activity the supervisor no longer retains still opens with its record.
  // biome-ignore lint/correctness/useExhaustiveDependencies: opens the record when the target changes, not when rows stream in
  useEffect(() => {
    if ("raw" in target) {
      ++inspection.current;
      setDetail(null);
      return;
    }
    if ("item" in target) {
      inspect(target.item).catch(report);
      return;
    }
    const { node } = target;
    const item = items.find((row) => row.id === node.activity_id);
    const receipt: ActivityDetail = {
      ...item,
      activity_id: node.activity_id,
      label: node.label || "Recorded event",
      status: node.state,
      memory: node.memory,
      command: node.command || item?.command,
      output: node.text,
    };
    if (item || node.memory) inspect(receipt).catch(report);
    else {
      ++inspection.current;
      setDetail(receipt);
      setError(null);
      setLoading(false);
    }
  }, [target]);

  // The parent stream already reports activity progress. Refresh an open
  // one only when that authoritative state changes instead of running a
  // second polling clock beside SSE.
  const rowsVersion = items
    .map((item) =>
      [item.id, item.status, item.progress, item.duration_ms].join(":"),
    )
    .join("|");
  // biome-ignore lint/correctness/useExhaustiveDependencies: refreshes when the rows' state (rowsVersion) or the open record's identity changes, not on each new detail object
  useEffect(() => {
    if (!detail || loading) return;
    inspect(current || detail, true).catch(report);
  }, [rowsVersion, detail?.id, loading]);

  const current =
    (detail && items.find((item) => item.id === detail.id)) || detail;
  const title =
    "raw" in target ? target.title : detail?.memory ? "Memory" : "Activity";
  const live = !!current && active(current);

  return (
    <Modal
      title={title}
      className="activity-view"
      layout="sheet"
      close={close}
      actions={
        live && (
          <ActivityControls
            item={current}
            name={title}
            session={session}
            online={online}
            report={report}
          />
        )
      }
    >
      {"raw" in target ? (
        <div class="activity-detail activity-page">
          <Raw {...target.raw} />
        </div>
      ) : detail && current ? (
        <DetailBody
          detail={detail}
          current={current}
          loading={loading}
          error={error}
          retry={() => inspect(detail)}
        />
      ) : (
        <Spinner surface />
      )}
    </Modal>
  );
}

function DetailBody({
  detail,
  current,
  loading,
  error,
  retry,
}: {
  detail: ActivityDetail;
  current: Activity;
  loading: boolean;
  error: unknown;
  retry: () => Promise<void>;
}) {
  const elapsed = useElapsed(current.duration_ms == null, current.started_ms);
  const meta = [
    current.status,
    current.started_ms && current.duration_ms != null
      ? duration(current.duration_ms)
      : elapsed,
  ].filter(Boolean);

  return (
    <div class="activity-detail">
      {error && <LoadError error={error} retry={retry} />}
      {meta.length > 0 && <p class="detail-meta small">{meta.join(" · ")}</p>}

      {detail.command ? (
        <section aria-label="Command">
          <pre class="detail-command" tabIndex={0}>
            {cleanText(detail.command)}
          </pre>
        </section>
      ) : (
        detail.label &&
        !detail.memory && <p class="detail-label">{detail.label}</p>
      )}

      {/* Each section holds its place from the first frame: content
          replaces the skeleton instead of pushing the layout around. */}
      {detail.memory ? (
        <section aria-label="Memory">
          <p class="detail-label">{detail.memory.key}</p>
          <p class="detail-meta small">
            Saved memory · it may have changed since this event.
          </p>
          {loading && !detail.output ? (
            <Skeleton label="Loading the memory…" />
          ) : (
            <Markdown text={detail.output || ""} />
          )}
        </section>
      ) : (
        <section aria-label="Output">
          {detail.output ? (
            <pre>{cleanText(detail.output)}</pre>
          ) : loading ? (
            <Skeleton label="Loading the output…" />
          ) : (
            <EmptyState>No output recorded.</EmptyState>
          )}
        </section>
      )}
    </div>
  );
}
