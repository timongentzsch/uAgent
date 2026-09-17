import type {
  Block,
  Exchange,
  Report,
  Session,
  Snapshot,
} from "../../shared/types.ts";
import type { RefObject } from "preact";
import { useCallback, useState } from "preact/hooks";
import { LoadError, Mark } from "../../shared/ui.tsx";
import { HistorySkeleton } from "../../shared/loading.tsx";
import { MessageRows } from "./message.tsx";
import { setAnchorMode } from "../../state/use-stick-to-bottom.ts";

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
  stopFollowing,
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
  stopFollowing: () => void;
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
  async function loadOlder() {
    if (loadingOlder) return;
    const box = scroller.current;
    const column = content.current;
    // The reader explicitly went to history: stop following first so the
    // prepend never yanks to the bottom. Anchor with viewport-relative
    // rects (not offsetTop, which depends on offsetParent and breaks with
    // content-visibility): the first visible row stays under the reader
    // across the prepend, whether it is an ARTICLE message or a DETAILS
    // tool row. Falls back to a height delta when the anchor is
    // gone (window cap trimmed it).
    stopFollowing();
    let anchor: HTMLElement | null = null;
    let anchorTop = 0;
    let boxTop = 0;
    let heightBefore = 0;
    let topBefore = 0;
    // Manual restore owns this prepend: suppress native overflow-anchoring
    // for the duration, otherwise the button label swap plus the prepend
    // each shift scroll and the manual correction double-applies.
    // The anchor lock forces real row heights (no content-visibility
    // estimates) so the math below is exact on the first frame instead
    // of chasing intrinsic sizes while rows resolve.
    const boxAnchor = box?.style.overflowAnchor;
    setAnchorMode(box, false);
    column?.classList.add("anchor-lock");
    if (box && column) {
      heightBefore = box.scrollHeight;
      topBefore = box.scrollTop;
      boxTop = box.getBoundingClientRect().top;
      for (const child of column.children) {
        const element = child as HTMLElement;
        if (!(element instanceof HTMLElement)) continue;
        if (!element.hasAttribute("data-message-id")) continue;
        const rect = element.getBoundingClientRect();
        if (rect.bottom <= boxTop + 1) continue;
        anchor = element;
        anchorTop = rect.top;
        break;
      }
    }
    setLoadingOlder(true);
    try {
      await older();
    } finally {
      setLoadingOlder(false);
    }
    // Restore after paint with one exact correction under lock (real
    // row heights, native anchoring suppressed), then release. Two
    // frames let the prepended render land before measuring.
    const target = anchorTop - boxTop;
    const read = () =>
      anchor?.isConnected && box
        ? anchor.getBoundingClientRect().top - box.getBoundingClientRect().top
        : NaN;
    const finish = () => {
      column?.classList.remove("anchor-lock");
      if (box) box.style.overflowAnchor = boxAnchor || "auto";
    };
    if (!anchor?.isConnected) {
      if (box && heightBefore > 0)
        box.scrollTop = topBefore + (box.scrollHeight - heightBefore);
      finish();
      return;
    }
    requestAnimationFrame(() =>
      requestAnimationFrame(() => {
        if (box && anchor?.isConnected) {
          const correction = read() - target;
          if (Number.isFinite(correction) && Math.abs(correction) >= 1)
            box.scrollTop += correction;
        }
        finish();
      }),
    );
  }
  const view = snapshot?.state?.view;
  const retry = () => loadSnapshot(selected).catch(() => {});
  return (
    <div
      class="transcript"
      data-session={selected}
      aria-busy={(!snapshot && !loadError) || undefined}
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
        {snapshot?.state?.error && (
          <p class="failure">{snapshot.state.error}</p>
        )}
      </div>
    </div>
  );
}
