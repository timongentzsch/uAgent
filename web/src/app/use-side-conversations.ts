import { useCallback } from "preact/hooks";
import type { Report, Session, Snapshot } from "../shared/types.ts";
import { stored, useStored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { command, requestId } from "../state/api.ts";

export type Side = { question: string; id?: string; firstRequest: string };

const kept = stored<Record<string, Side>>(
  storageKeys.sideConversations,
  {},
  undefined,
  {
    kept: (value) =>
      Object.fromEntries(Object.entries(value).filter(([, side]) => side.id)),
  },
);

// The /btw side conversation of each conversation: started from a question,
// kept across reloads once it has its own session, and asked again there.
export function useSideConversations({
  selected,
  load,
  refresh,
  report,
}: {
  selected: string;
  load: (id: string) => Promise<Snapshot>;
  refresh: () => Promise<void>;
  report: Report;
}) {
  const [sides, setSides] = useStored(kept);
  const side = sides[selected] || null;
  const closeSide = useCallback(
    () =>
      setSides((current) => {
        const next = { ...current };
        delete next[selected];
        return next;
      }),
    [selected],
  );
  // Asks the parent's side conversation, starting it with the first one.
  async function ask(parent: Session, argument: string) {
    const existing = sides[parent.id];
    if (existing && !existing.id)
      throw new Error("The side conversation is starting.");
    if (existing?.id) {
      const snapshot = await load(existing.id);
      await command("submit", snapshot.metadata, { text: argument });
      return;
    }
    const firstRequest = requestId();
    setSides((current) => ({
      ...current,
      [parent.id]: { question: argument, firstRequest },
    }));
    // The side owns an ordinary runtime: tool dispatch, approvals and
    // follow-ups have one implementation, separate from the parent turn.
    void (async () => {
      const result = await command("side", parent, {
        operation: "create",
        text: argument,
      });
      if (result.pending || !result.result.id)
        throw new Error(
          "The side conversation could not be started. Refresh to inspect it.",
        );
      const id = result.result.id;
      await refresh();
      await command("activate", { id, generation: "" });
      const snapshot = await load(id);
      setSides((current) =>
        current[parent.id]?.firstRequest === firstRequest
          ? {
              ...current,
              [parent.id]: { question: argument, id, firstRequest },
            }
          : current,
      );
      await command("submit", snapshot.metadata, {
        text: argument,
        request_id: firstRequest,
      });
    })().catch((failure) => {
      setSides((current) => {
        if (current[parent.id]?.firstRequest !== firstRequest) return current;
        const next = { ...current };
        delete next[parent.id];
        return next;
      });
      report(failure);
    });
  }
  // Logging out forgets them all.
  const clearSides = () => setSides({});
  return { side, closeSide, ask, clearSides };
}
