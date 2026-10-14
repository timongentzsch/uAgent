import { useState, type Dispatch, type StateUpdater } from "preact/hooks";
import type {
  Act,
  AppModal,
  CommandFields,
  Draft,
  Session,
  SlashCommand,
} from "../shared/types.ts";
import { failure } from "../shared/types.ts";
import { QUEUED_NEXT } from "../shared/message-view.ts";
import { emptyDraft } from "../state/store.ts";
import { requestId } from "../state/api.ts";
import type { useHost } from "../state/use-host.ts";
import type { Side } from "./use-overlays.ts";
import { parseSlash } from "../features/composer/slash.ts";
import {
  restartConversation,
  shareTranscript,
} from "../features/sidebar/sidebar.tsx";

type Host = ReturnType<typeof useHost>;

interface SendInputs extends Pick<
  Host,
  "snapshots" | "setDrafts" | "setOutgoing" | "patchOutgoing" | "report"
> {
  online: boolean;
  selected: string;
  session: Session | undefined;
  draft: Draft;
  uploading: boolean;
  running: boolean;
  commands: SlashCommand[];
  levels: string[];
  act: Act;
  // A command with a screen opens it when typed bare.
  screens: Record<string, () => void>;
  startConversation: (
    folder: string,
    panel?: boolean,
    space?: string,
  ) => Promise<string | undefined>;
  forkAndOpen: (
    target: Session | undefined,
    fields?: CommandFields,
    edit?: boolean,
  ) => Promise<void>;
  changeDetail: (level: string) => Promise<void>;
  updateDraft: (id: string, update: (draft: Draft) => Draft) => void;
  setNotice: (notice: string) => void;
  setModal: (modal: AppModal | null) => void;
  setSide: Dispatch<StateUpdater<Side | null>>;
}

// What the composer sends: a slash command that runs in the app, or a
// message that goes out as a row of its own.
export function useSend({
  snapshots,
  setDrafts,
  setOutgoing,
  patchOutgoing,
  report,
  online,
  selected,
  session,
  draft,
  uploading,
  running,
  commands,
  levels,
  act,
  screens,
  startConversation,
  forkAndOpen,
  changeDetail,
  updateDraft,
  setNotice,
  setModal,
  setSide,
}: SendInputs) {
  const [busy, setBusy] = useState(false);
  async function localCommand(text: string) {
    const { name, argument } = parseSlash(commands, text);
    if (!argument && screens[name]) screens[name]();
    else if (name === "/reset") await startConversation(session!.cwd!);
    else if (name === "/verbosity") {
      if (!levels.includes(argument))
        throw new Error(`Use /verbosity ${levels.join("|")}`);
      await changeDetail(argument);
    }
    // In a terminal /quit detaches and the runtime stays; here that is just
    // leaving the page, so it never closes anything.
    else if (name === "/quit")
      throw new Error("Use Close conversation in the conversation menu.");
    else if (name === "/restart") {
      setNotice(await restartConversation(session!));
    } else if (name === "/fork") {
      await forkAndOpen(session, { argument });
    } else if (name === "/rewind" && argument) {
      // Bare /rewind runs on the host and lists the message numbers.
      await forkAndOpen(session, { argument }, true);
    } else if (name === "/btw") {
      if (!argument) throw new Error("Use /btw QUESTION");
      // The card shows the question; the composer is free again at once.
      setSide({ question: argument });
      act("side", { text: argument }).then(
        (result) => {
          if (!result.pending)
            setSide((current) =>
              current?.question === argument
                ? { question: argument, answer: result.result.answer }
                : current,
            );
        },
        (failure) => {
          setSide(null);
          report(failure);
        },
      );
    } else if (name === "/share") {
      setNotice(await shareTranscript(session!));
    } else if (name === "/instructions" && !argument) {
      setModal({ type: "instructions" });
    } else if (name === "/http") {
      const exchanges = snapshots.get()[selected]?.state?.http || [];
      const [number, part = "request"] = argument.split(/\s+/);
      const index = number ? Number(number) : exchanges.length;
      if (
        !Number.isInteger(index) ||
        index < 1 ||
        index > exchanges.length ||
        !["request", "response"].includes(part)
      )
        throw new Error(
          exchanges.length
            ? "Use /http INDEX request|response"
            : "No HTTP exchange captured",
        );
      setModal({
        type: "raw",
        session: selected,
        exchanges: [exchanges[index - 1]],
        part: part as "request" | "response",
      });
    } else return false;
    return true;
  }
  async function submit(event: Event, jumpToLatest: () => void, queue = false) {
    event.preventDefault();
    const pending = snapshots.get()[selected]?.pending;
    setNotice("");
    if (
      !online ||
      pending ||
      (!draft.text.trim() && !draft.files.length) ||
      uploading ||
      busy
    )
      return;
    setBusy(true);
    // Sending always produces bottom content (optimistic message, command
    // output, streamed answer): re-stick synchronously so the turn stays
    // attached even if the stick was lost while reading history or the
    // send-frame layout churn (composer shrink, optimistic append) moves
    // scroll before the observers re-pin. Idempotent when already sticky.
    jumpToLatest();
    const id = selected,
      sent = draft,
      request_id = requestId();
    if (sent.text.startsWith("/")) {
      try {
        // A side question is the one command meant for a running turn.
        if (running && !sent.text.startsWith("/btw "))
          throw new Error(
            "Wait for this turn to finish before running a slash command.",
          );
        if (await localCommand(sent.text)) {
          setDrafts((current) =>
            current[id] === sent
              ? { ...current, [id]: { ...sent, text: "" } }
              : current,
          );
          setBusy(false);
          return;
        }
        if (
          parseSlash(commands, sent.text).name === "/attach" &&
          sent.text.trim().endsWith(" clear")
        )
          updateDraft(id, emptyDraft);
      } catch (error) {
        report(error);
        setBusy(false);
        return;
      }
    }
    await deliver(id, sent, queue, request_id);
  }
  // A message goes out as a row of its own until the host confirms it; a
  // failed one stays there, with Retry and Return to composer. Guidance
  // joins the running turn, or with `queue` waits for it to end.
  async function deliver(
    id: string,
    sent: Draft,
    queue = false,
    request_id = requestId(),
  ) {
    setBusy(true);
    const kind = running && !snapshots.get()[id]?.pending ? "steer" : "submit";
    const queued =
      kind === "steer" && (queue ? QUEUED_NEXT : "Guidance queued");
    const visible = !sent.text.startsWith("/") || sent.files.length;
    if (visible)
      setOutgoing((items) => [
        ...items,
        {
          id: `outgoing-${request_id}`,
          request_id,
          session_id: id,
          kind: "user",
          text: sent.text,
          files: sent.files,
          time: new Date().toISOString(),
          status: queued || "Sending…",
        },
      ]);
    setDrafts((current) =>
      current[id] === sent ? { ...current, [id]: emptyDraft() } : current,
    );
    try {
      const result = await act(kind, {
        request_id,
        text: sent.text,
        ...(queued && queue && { queue }),
        attachment_ids: sent.files.map((item) => ({
          id: item.id,
          name: item.name,
        })),
      });
      patchOutgoing(request_id, {
        status: result.pending ? "Awaiting confirmation" : queued || "Sent",
      });
    } catch (error) {
      const issue = failure(error);
      if (visible)
        patchOutgoing(request_id, {
          status: issue.rejected ? "Not sent" : "Not confirmed",
          error: issue.message,
        });
      else report(issue);
    } finally {
      setBusy(false);
    }
  }
  return { busy, setBusy, localCommand, submit, deliver };
}
