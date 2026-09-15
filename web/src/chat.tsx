import type { Block, Exchange, Report, Session, Snapshot } from "./types.ts";
import type { RefObject } from "preact";
import { LoadError, Mark, Skeleton } from "./ui.tsx";
import { MessageRows } from "./message.tsx";

export default function Chat({
  scroller,
  content,
  sentinel,
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
}: {
  scroller: RefObject<HTMLDivElement>;
  content: RefObject<HTMLDivElement>;
  sentinel: RefObject<HTMLDivElement>;
  selected: string;
  snapshot?: Snapshot;
  loadError?: unknown;
  blocks: Block[];
  session: Session;
  online: boolean;
  loadSnapshot: (id: string) => Promise<Snapshot>;
  older: () => Promise<void>;
  report: Report;
  recall: (block: Block) => void;
  inspect: (id: string) => void;
  http: (exchanges: Exchange[]) => void;
  activity: (block: Block) => void;
  statistics: (block: Block) => void;
}) {
  const view = snapshot?.state?.view;
  const retry = () => loadSnapshot(selected).catch(() => {});
  return (
    <div
      class="transcript"
      data-session={selected}
      aria-busy={(!snapshot && !loadError) || undefined}
      ref={scroller}
    >
      <div class="transcript-content" ref={content}>
      {view?.more && (
        <button
          class="history-button"
          disabled={!online}
          onClick={() => older().catch(report)}
        >
          Load older retained messages
        </button>
      )}
      {(view?.dropped_segments || 0) > 0 && (
        <p class="retention">
          {view?.dropped_segments} older segments are outside retention.
        </p>
      )}
      {snapshot?.live_truncated && (
        <p class="retention">
          The live preview exceeded its buffer. Retained history refreshes when
          this turn saves.
        </p>
      )}
      {!snapshot &&
        (loadError ? (
          <LoadError error={loadError} retry={retry} />
        ) : (
          <Skeleton
            className="history-skeleton"
            rows={8}
            label="Loading conversation…"
          />
        ))}
      {snapshot && loadError && <LoadError error={loadError} retry={retry} />}
      {snapshot && blocks.length === 0 && (
        <div class="empty">
          <Mark className="cursor-mark" />
          <h2>What are we working on?</h2>
          <p>
            Describe a task, attach a file, or use an existing slash command.
          </p>
        </div>
      )}
      {snapshot && (
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
      {snapshot?.state?.error && <p class="failure">{snapshot.state.error}</p>}
      <div ref={sentinel} class="transcript-sentinel" aria-hidden="true" />
      </div>
    </div>
  );
}
