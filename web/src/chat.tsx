import type { Block, Exchange, Report, Session, Snapshot } from "./types.ts";
import type { RefObject } from "preact";
import { useState } from "preact/hooks";
import { LoadError, Mark } from "./ui.tsx";
import { HistorySkeleton } from "./loading.tsx";
import { MessageRows } from "./message.tsx";

export default function Chat({
  scroller,
  content,
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
    // exploration group. Falls back to a height delta when the anchor is
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
    if (box) box.style.overflowAnchor = "none";
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
    // Restore after paint with a closed loop. The locked phase runs under
    // the anchor lock (real row heights, native anchoring suppressed) and
    // corrects the anchor to its pre-load box-relative target. It then
    // releases the lock and native anchoring and keeps holding the target
    // while late layout (skipped rows settling, fonts, composer chrome)
    // quiets down. Without the hold, the unlock ripple alone can leave
    // the anchor ~a row off with nobody left to correct it.
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
    let locked = true;
    let frames = 0;
    let stable = 0;
    const step = () => {
      requestAnimationFrame(() => {
        if (!box || !anchor?.isConnected) {
          finish();
          return;
        }
        const correction = read() - target;
        const off = Number.isFinite(correction) && Math.abs(correction) >= 1;
        if (locked) {
          // Exact settle while measurements are trustworthy.
          if (off && frames < 3) {
            box.scrollTop += correction;
            frames += 1;
            step();
            return;
          }
          // Release the lock, then prove the position holds.
          locked = false;
          frames = 0;
          stable = 0;
          finish();
          step();
          return;
        }
        if (off && frames < 6) {
          box.scrollTop += correction;
          frames += 1;
          stable = 0;
          step();
          return;
        }
        // Already unlocked: stop after two consecutive stable frames.
        if (!off) stable += 1;
        if (stable >= 2 || frames >= 6) return;
        frames += 1;
        step();
      });
    };
    step();
  }
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
          type="button"
          class="history-button"
          disabled={!online || loadingOlder}
          aria-busy={loadingOlder || undefined}
          onClick={() => loadOlder().catch(report)}
        >
          {loadingOlder ? "Loading older messages…" : "Load older retained messages"}
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
      {snapshot?.state?.error && <p class="failure">{snapshot.state.error}</p>}
      </div>
    </div>
  );
}
