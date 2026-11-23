import { useMemo, useRef, useState } from "preact/hooks";
import type { Session } from "../shared/types.ts";
import { stored, useStored } from "../shared/storage.ts";
import { storageKeys } from "../shared/storage-keys.ts";
import { requestId, useApi } from "../state/api.ts";
import { isSideChat } from "../state/attention.ts";
import type { HostState } from "../state/use-host.ts";

// A tab: a side chat, keyed by its conversation, or one being asked, keyed
// by its first request until it has one.
export type Side = {
  key: string;
  question: string;
  request: string;
  id?: string;
};
// What is being asked about: for the side chat named, or for a new one.
export type Asking = { to?: string; quote: string };
// Which of a conversation's side chats this device shows as tabs, and the one
// in front; "" is the new one being asked. Which side chats there are is the
// host's.
type Open = { tabs: string[]; front: string };
type Starting = Side & { parent: string };

const CLOSED: Open = { tabs: [], front: "" };

const kept = stored<Record<string, Open>>(storageKeys.sideTabs, {}, (value) =>
  Object.fromEntries(
    Object.entries(value || {}).filter(([, open]) => Array.isArray(open?.tabs)),
  ),
);

// A conversation's side chats, in the order asked.
const sidesOf = (sessions: Session[], parent: string) =>
  sessions
    .filter((item) => item.parent === parent && isSideChat(item))
    .sort((a, b) => a.side!.asked - b.side!.asked);

export function useSideConversations({
  selected,
  catalogue,
  load,
  refresh,
  report,
}: HostState) {
  const { command } = useApi();
  const [all, setAll] = useStored(kept);
  const [asked, setAsked] = useState<Asking & { parent: string }>();
  const [starting, setStarting] = useState<Starting[]>([]);
  // A side chat hidden while it was being asked gets no tab once it is there.
  const dropped = useRef(new Set<string>());
  const asking = asked?.parent === selected ? asked : undefined;
  const list = useMemo(
    () => sidesOf(catalogue.sessions, selected),
    [catalogue.sessions, selected],
  );
  const requests = useMemo(
    () => starting.map((side) => side.request),
    [starting],
  );
  const open = all[selected] || CLOSED;
  const sides: Side[] = [
    ...list
      .filter((item) => open.tabs.includes(item.id))
      .map((item) => ({
        key: item.id,
        id: item.id,
        question: item.title || "Side chat",
        request: item.side!.request,
      })),
    ...starting.filter((side) => side.parent === selected),
  ];
  const shown = sides.some((side) => side.key === open.front)
    ? open.front
    : asking && !asking.to
      ? ""
      : (sides.at(-1)?.key ?? "");
  const change = (parent: string, to: (open: Open) => Open) =>
    setAll((current) => ({
      ...current,
      [parent]: to(current[parent] || CLOSED),
    }));
  // Its tab, in front; `parent` when it is not the open conversation's.
  const show = (key: string, parent = selected) =>
    change(parent, (open) => ({
      tabs: !key || open.tabs.includes(key) ? open.tabs : [...open.tabs, key],
      front: key,
    }));
  const cancel = () => setAsked(undefined);
  // Its tab goes; the side chat stays.
  function hide(key: string) {
    if ((asking?.to || "") === key) cancel();
    if (starting.some((side) => side.key === key)) {
      dropped.current.add(key);
      setStarting((now) => now.filter((side) => side.key !== key));
    }
    change(selected, (open) => ({
      ...open,
      tabs: open.tabs.filter((tab) => tab !== key),
    }));
  }
  function quote(text: string, to?: string) {
    setAsked({ parent: selected, to, quote: text });
    show(to || "");
  }
  // Every question starts a side chat of its own; resolves to its
  // conversation, or to nothing when it could not be started.
  function ask(parent: Session, question: string, quoted = "") {
    const request = requestId();
    setStarting((now) => [
      ...now,
      { key: request, question, request, parent: parent.id },
    ]);
    change(parent.id, (open) => ({ ...open, front: request }));
    // The side owns an ordinary runtime: tool dispatch, approvals and
    // follow-ups have one implementation, separate from the parent turn.
    return (async () => {
      const result = await command("side", parent, {
        operation: "create",
        text: question,
        request,
      });
      if (result.pending || !result.result.id)
        throw new Error(
          "The side chat could not be started. Refresh to inspect it.",
        );
      const id = result.result.id as string;
      await refresh();
      await command("activate", { id, generation: "" });
      const snapshot = await load(id);
      await command(
        "submit",
        snapshot.metadata,
        {
          text: quoted ? `${quoted}\n\n${question}` : question,
          request_id: request,
        },
        { streamed: true },
      );
      // Shown once its turn has started and its snapshot says so: the stream
      // put the turn's start before the outcome, and this read follows both. It
      // only makes the first render right: where it fails, its pane reads again.
      await load(id).catch(() => undefined);
      if (!dropped.current.has(request))
        change(parent.id, (open) => ({
          tabs: [...open.tabs, id],
          front: open.front === request ? id : open.front,
        }));
      return id as string | undefined;
    })()
      .catch((failure) => {
        report(failure);
        return undefined;
      })
      .finally(() => {
        dropped.current.delete(request);
        setStarting((now) => now.filter((side) => side.key !== request));
      });
  }
  // It leaves the side chats for the list of conversations.
  async function detach(item: Session) {
    await command("side", item, { operation: "detach" });
    // The list knows it is a conversation of its own before it is opened.
    await refresh();
    hide(item.id);
  }
  const clearSides = () => setAll({});
  return {
    list,
    starting: requests,
    sides,
    shown,
    asking,
    show,
    hide,
    cancel,
    quote,
    ask,
    detach,
    clearSides,
  };
}
