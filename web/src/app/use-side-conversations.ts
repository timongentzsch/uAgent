import { useState } from "preact/hooks";
import type { Session } from "../shared/types.ts";
import { stored, useStored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { requestId, useApi } from "../state/api.ts";
import type { HostState } from "../state/use-host.ts";

export type Side = { question: string; id?: string; firstRequest: string };
// What is being asked about: for the side chat named, or for a new one.
export type Asking = { to?: string; quote: string };
// A conversation's side chats and the one shown, by its first request;
// "" is the new one being asked.
type Open = { list: Side[]; shown: string };

const NONE: Side[] = [];

// Session ids and side conversations are main-owned, across execution hosts.
const kept = stored<Record<string, Open>>(
  storageKeys.sideConversations,
  {},
  (value) =>
    Object.fromEntries(
      Object.entries(value || {}).filter(([, open]) =>
        Array.isArray(open?.list),
      ),
    ),
  {
    kept: (value) =>
      Object.fromEntries(
        Object.entries(value)
          .map(
            ([parent, open]) =>
              [
                parent,
                { ...open, list: open.list.filter((side) => side.id) },
              ] as const,
          )
          .filter(([, open]) => open.list.length),
      ),
  },
);

export function useSideConversations({
  selected,
  load,
  refresh,
  report,
}: HostState) {
  const { command } = useApi();
  const [all, setAll] = useStored(kept);
  const [asked, setAsked] = useState<Asking & { parent: string }>();
  const asking = asked?.parent === selected ? asked : undefined;
  const open = all[selected];
  const sides = open?.list || NONE;
  const shown = sides.some((side) => side.firstRequest === open?.shown)
    ? open!.shown
    : asking && !asking.to
      ? ""
      : (sides.at(-1)?.firstRequest ?? "");
  const change = (parent: string, to: (open: Open) => Open) =>
    setAll((current) => ({
      ...current,
      [parent]: to(current[parent] || { list: [], shown: "" }),
    }));
  const show = (key: string) =>
    change(selected, (open) => ({ ...open, shown: key }));
  const cancel = () => setAsked(undefined);
  // The new one closes unasked; a started one stays a conversation.
  function close(key: string, parent = selected) {
    if ((asking?.to || "") === key) cancel();
    change(parent, (open) => ({
      ...open,
      list: open.list.filter((side) => side.firstRequest !== key),
    }));
  }
  // The conversation this one is a side chat of, where this device asked it.
  const origin = Object.entries(all).flatMap(([parent, { list }]) =>
    list
      .filter((side) => side.id === selected)
      .map((side) => ({ parent, key: side.firstRequest })),
  )[0];
  function quote(text: string, to?: string) {
    setAsked({ parent: selected, to, quote: text });
    show(to || "");
  }
  // Every question starts a side chat of its own; resolves to its
  // conversation, or to nothing when it could not be started.
  function ask(parent: Session, question: string, quoted = "") {
    const firstRequest = requestId();
    change(parent.id, (open) => ({
      list: [...open.list, { question, firstRequest }],
      shown: firstRequest,
    }));
    // The side owns an ordinary runtime: tool dispatch, approvals and
    // follow-ups have one implementation, separate from the parent turn.
    return (async () => {
      const result = await command("side", parent, {
        operation: "create",
        text: question,
      });
      if (result.pending || !result.result.id)
        throw new Error(
          "The side chat could not be started. Refresh to inspect it.",
        );
      const id = result.result.id;
      await refresh();
      await command("activate", { id, generation: "" });
      const snapshot = await load(id);
      await command(
        "submit",
        snapshot.metadata,
        {
          text: quoted ? `${quoted}\n\n${question}` : question,
          request_id: firstRequest,
        },
        { streamed: true },
      );
      // Shown once its turn has started and its snapshot says so: the stream
      // put the turn's start before the outcome, and this read follows both.
      await load(id);
      change(parent.id, (open) => ({
        ...open,
        list: open.list.map((side) =>
          side.firstRequest === firstRequest ? { ...side, id } : side,
        ),
      }));
      return id as string | undefined;
    })().catch((failure) => {
      change(parent.id, (open) => ({
        ...open,
        list: open.list.filter((side) => side.firstRequest !== firstRequest),
      }));
      report(failure);
      return undefined;
    });
  }
  const clearSides = () => setAll({});
  return {
    sides,
    shown,
    asking,
    origin,
    show,
    close,
    cancel,
    quote,
    ask,
    clearSides,
  };
}
