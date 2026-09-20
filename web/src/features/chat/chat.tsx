import type {
  Block,
  Exchange,
  Report,
  Session,
  Snapshot,
} from "../../shared/types.ts";
import type { RefObject } from "preact";
import { useCallback, useEffect, useState } from "preact/hooks";
import { LoadError, Mark } from "../../shared/ui.tsx";
import { HistorySkeleton } from "../../shared/loading.tsx";
import { MessageRows, prepareHistoryBlocks } from "./message.tsx";

export { prepareHistoryBlocks } from "./message.tsx";

export default function Chat({
  scroller,
  content,
  attachScroller,
  attachContent,
  selected,
  snapshot,
  loadError,
  blocks,
  session,
  online,
  loadSnapshot,
  older,
  report,
  recall,
  inspect,
  http,
  activity,
  statistics,
  preserveWhile,
}: {
  scroller: RefObject<HTMLDivElement>;
  content: RefObject<HTMLDivElement>;
  attachScroller: (node: HTMLDivElement | null) => void;
  attachContent: (node: HTMLDivElement | null) => void;
  selected: string;
  snapshot?: Snapshot;
  loadError?: unknown;
  blocks: Block[];
  session: Session;
  online: boolean;
  loadSnapshot: (id: string) => Promise<Snapshot>;
  older: () => Promise<void>;
  preserveWhile: (load: () => Promise<void>) => Promise<void>;
  report: Report;
  recall: (block: Block) => void;
  inspect: (id: string) => void;
  http: (exchanges: Exchange[]) => void;
  activity: (block: Block) => void;
  statistics: (block: Block) => void;
}) {
  // Keep the hook's mirrors current and rebind its node-keyed effects
  // on every remount; both callbacks are stable, so no ref churn.
  const attachBox = useCallback(
    (node: HTMLDivElement | null) => {
      scroller.current = node;
      attachScroller(node);
    },
    [scroller, attachScroller],
  );
  const attachColumn = useCallback(
    (node: HTMLDivElement | null) => {
      content.current = node;
      attachContent(node);
    },
    [content, attachContent],
  );
  const [loadingOlder, setLoadingOlder] = useState(false);
  const [preparedKey, setPreparedKey] = useState("");
  const prepared = !!snapshot && preparedKey === selected;
  useEffect(() => {
    if (!snapshot) return;
    let active = true;
    prepareHistoryBlocks(snapshot.state?.view?.blocks || [])
      .catch(() => {}) // The message renderer shows its own retryable error.
      .finally(() => {
        if (active) setPreparedKey(selected);
      });
    return () => {
      active = false;
    };
  }, [selected, !!snapshot]);
  async function loadOlder() {
    if (loadingOlder) return;
    setLoadingOlder(true);
    try {
      await preserveWhile(older);
    } finally {
      setLoadingOlder(false);
    }
  }
  const view = snapshot?.state?.view;
  const retry = () => loadSnapshot(selected).catch(() => {});
  return (
    <div
      class="transcript"
      data-session={selected}
      data-history-key={`chat:${selected}`}
      aria-busy={
        (!snapshot && !loadError) || (snapshot && !prepared) || undefined
      }
      ref={attachBox}
    >
      <div class="transcript-content" ref={attachColumn}>
        {view?.more && (
          <button
            type="button"
            class="history-button"
            disabled={!online || loadingOlder}
            aria-busy={loadingOlder || undefined}
            onClick={() => loadOlder().catch(report)}
          >
            {loadingOlder
              ? "Loading older messages…"
              : "Load older retained messages"}
          </button>
        )}
        {(view?.dropped_segments || 0) > 0 && (
          <p class="retention">
            {view?.dropped_segments} older segments are outside retention.
          </p>
        )}
        {snapshot?.live_truncated && (
          <p class="retention">
            The live preview exceeded its buffer. Retained history refreshes
            when this turn saves.
          </p>
        )}
        {!snapshot &&
          (loadError ? (
            <LoadError error={loadError} retry={retry} />
          ) : (
            <HistorySkeleton />
          ))}
        {snapshot && loadError && <LoadError error={loadError} retry={retry} />}
        {snapshot && !prepared && <HistorySkeleton />}
        {snapshot && prepared && blocks.length === 0 && (
          <div class="empty">
            <Mark className="cursor-mark" />
            <h2>What are we working on?</h2>
            <p>
              Describe a task, attach a file, or use an existing slash command.
            </p>
          </div>
        )}
        {snapshot && prepared && (
          <MessageRows
            blocks={blocks}
            online={online}
            session={session}
            report={report}
            recall={recall}
            inspect={inspect}
            http={http}
            activity={activity}
            statistics={statistics}
          />
        )}
        {session.error && <p class="failure">{session.error}</p>}
        {snapshot?.state?.error && (
          <p class="failure">{snapshot.state.error}</p>
        )}
      </div>
    </div>
  );
}
