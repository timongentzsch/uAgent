import { useCallback, useMemo } from "preact/hooks";
import type { StateUpdater } from "preact/hooks";
import type {
  Act,
  ActivityTarget,
  AppModal,
  CommandFields,
  Draft,
  Exchange,
  InspectorTarget,
  Node,
  Outgoing,
  Report,
  Session,
} from "../shared/types.ts";
import { failure } from "../shared/types.ts";
import {
  outgoingStatus,
  queuedGuidance,
  recallable,
  unsent,
} from "../shared/message-view.ts";
import { useLatest } from "../shared/use-latest.ts";
import type { SnapshotStore } from "../state/snapshot-store.ts";
import { transcriptNodes } from "./chat-page.tsx";

// What a message's controls do: retry, continue, branch, recall or insert
// guidance, and open what a row links to.
// Every row reads them through one context value, so they stay the same
// functions and read the latest state through a ref.
export function useMessageActions({
  online,
  selected,
  act,
  report,
  forkAndOpen,
  outgoing,
  session,
  deliver,
  busy,
  setOutgoing,
  patchOutgoing,
  updateDraft,
  snapshots,
  setInspector,
  setModal,
}: {
  online: boolean;
  selected: string;
  act: Act;
  report: Report;
  forkAndOpen: (
    target: Session | undefined,
    fields?: CommandFields<"fork">,
    edit?: boolean,
  ) => Promise<void>;
  outgoing: Outgoing[];
  session?: Session;
  deliver: (id: string, sent: Draft) => Promise<void>;
  busy: boolean;
  setOutgoing: (update: StateUpdater<Outgoing[]>) => void;
  patchOutgoing: (requestId: string, patch: Partial<Outgoing>) => void;
  updateDraft: (id: string, change: (draft: Draft) => Draft) => void;
  snapshots: SnapshotStore;
  setInspector: (target: InspectorTarget | null) => void;
  setModal: (modal: AppModal | null) => void;
}) {
  const latest = useLatest({
    online,
    selected,
    act,
    report,
    forkAndOpen,
    outgoing,
    session,
    deliver,
    busy,
  });
  // A failed message sends again as a new request; Continue resumes a
  // stopped turn. Both leave the composer's draft alone.
  const retrySend = useCallback(
    (node: Node) => {
      const { online, selected, deliver, busy } = latest.current;
      if (!online || busy || !node.request_id || !unsent(node)) return;
      setOutgoing((items) =>
        items.filter((item) => item.request_id !== node.request_id),
      );
      void deliver(selected, {
        text: node.body?.text || "",
        files: node.files || [],
      });
    },
    [setOutgoing],
  );
  const resume = useCallback(() => {
    const { online, selected, deliver, busy } = latest.current;
    if (online && !busy)
      void deliver(selected, { text: "continue", files: [] });
  }, []);
  // From a message's menu: edit it in a fork (fork before it), or keep it
  // and its reply (fork before the next message of yours).
  const branchFrom = useCallback(
    (node: Pick<Node, "id">, edit: boolean) => {
      const { forkAndOpen, outgoing, selected, report } = latest.current;
      const nodes = transcriptNodes(
        snapshots.get()[selected]?.state?.view,
        outgoing,
        selected,
      );
      const after = nodes.slice(
        nodes.findIndex((item) => item.id === node.id) + 1,
      );
      const next = after.find((item) => item.kind === "turn");
      forkAndOpen(
        latest.current.session,
        edit ? { message_id: node.id } : next ? { message_id: next.id } : {},
        edit,
      ).catch(report);
    },
    [snapshots],
  );
  // Recall returns queued guidance to the composer while it is still
  // queued. Delivered guidance belongs to the turn; dropping the row is
  // then the only correct move. A send that failed returns the same way,
  // with nothing to withdraw from the host.
  const recallGuidance = useCallback(
    async (node: Node) => {
      const { online, selected, act, report } = latest.current;
      const target = node.request_id;
      if (!target || !recallable(node, online)) return;
      const queued = queuedGuidance(node);
      const text = node.body?.text || "";
      const id = selected;
      if (queued) {
        try {
          await act("recall", { target_id: target });
        } catch (error) {
          const issue = failure(error);
          if (!/already delivered/i.test(issue.message)) {
            report(error);
            return;
          }
          // Delivery won the race: returning it would duplicate the input.
          setOutgoing((items) =>
            items.filter((item) => item.request_id !== target),
          );
          return;
        }
      }
      setOutgoing((items) =>
        items.filter((item) => item.request_id !== target),
      );
      updateDraft(id, (draft) => ({
        ...draft,
        text: text
          ? draft.text
            ? `${draft.text}\n${text}`
            : text
          : draft.text,
        files: [
          ...draft.files,
          ...(node.files || []).filter(
            (file) => !draft.files.some((current) => current.id === file.id),
          ),
        ],
      }));
    },
    [setOutgoing, updateDraft],
  );
  const insertGuidance = useCallback(
    async (node: Node) => {
      const { online, act, report } = latest.current;
      if (!online || !node.request_id || !queuedGuidance(node)) return;
      try {
        await act("insert", { target_id: node.request_id });
        // Keep the tray until the transcript acknowledges actual delivery.
        patchOutgoing(node.request_id, { status: outgoingStatus.guidance });
      } catch (error) {
        if (!/already delivered/i.test(failure(error).message)) report(error);
      }
    },
    [patchOutgoing],
  );
  return useMemo(
    () => ({
      report,
      recall: recallGuidance,
      insert: insertGuidance,
      retry: retrySend,
      resume,
      branch: branchFrom,
      inspect: (id: string) =>
        setInspector({
          title: id.startsWith("t-") ? "Tool input/output" : "Full content",
          raw: { id, session: selected },
        }),
      http: (exchanges: Exchange[]) =>
        setModal({ type: "raw", session: selected, exchanges }),
      activity: (node: ActivityTarget) => setInspector({ node }),
      statistics: (node: Node) =>
        setModal({
          type: "statistics",
          session_id: selected,
          node_id: node.id,
          unit: node.kind === "footer" ? "Turn" : "Message",
        }),
    }),
    [
      report,
      recallGuidance,
      insertGuidance,
      retrySend,
      resume,
      branchFrom,
      selected,
      setInspector,
      setModal,
    ],
  );
}
