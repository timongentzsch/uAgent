import type {
  Act,
  AppModal,
  CommandFields,
  Draft,
  Session,
  SlashCommand,
} from "../shared/types.ts";
import { failure } from "../shared/types.ts";
import { emptyDraft } from "../state/store.ts";
import { command, requestId } from "../state/api.ts";
import type { HostState } from "../state/use-host.ts";
import { outgoingStatus } from "../shared/message-view.ts";
import { folderOf } from "../shared/display.ts";
import { restartConversation, shareTranscript } from "./menus.tsx";
import { parseSlash } from "../features/composer/slash.ts";
import { focusComposer } from "../shared/navigation.ts";

// Runs what the composer and the command palette send: the shell calls this
// with its state and gets back submit, guidance and slash-command handlers.

type Page = "chat" | "library" | "scheduled";

// What the composer sends and the palette runs: a message, guidance for the
// running turn, or a slash command, run here when it has a screen or acts
// on this page and on the host otherwise.
export function useCommands({
  host,
  session,
  running,
  busy,
  setBusy,
  draft,
  uploading,
  setDraft,
  updateDraft,
  act,
  choose,
  startConversation,
  setModal,
  open,
  setPage,
  setLibraryKind,
  compact,
  setDrawer,
  search,
  setNotice,
  levels,
  changeDetail,
  askSide,
}: {
  host: HostState;
  session?: Session;
  running: boolean;
  busy: boolean;
  setBusy: (busy: boolean) => void;
  draft: Draft;
  uploading: boolean;
  setDraft: (value: Draft, id?: string) => void;
  updateDraft: (id: string, change: (draft: Draft) => Draft) => void;
  act: Act;
  choose: (id: string) => Promise<void>;
  startConversation: (
    folder: string,
    panel?: boolean,
    space?: string,
  ) => Promise<string | undefined>;
  setModal: (modal: AppModal | null) => void;
  open: (modal: AppModal) => void;
  setPage: (page: Page) => void;
  setLibraryKind: (kind: "memory" | "skills") => void;
  compact: boolean;
  setDrawer: (open: boolean) => void;
  // Opens search over everything.
  search: () => void;
  setNotice: (notice: string) => void;
  levels: string[];
  changeDetail: (level: string) => Promise<void>;
  askSide: (parent: Session, question: string) => Promise<void>;
}) {
  const {
    catalogue,
    snapshots,
    selected,
    online,
    setDrafts,
    setOutgoing,
    patchOutgoing,
    report,
    refresh,
    load,
  } = host;
  // A command with a screen opens it when typed bare; with an argument it
  // runs on the host, as in the terminal.
  const screens: Record<string, () => void> = {
    "/context": () => showContext(),
    "/config": () => open({ type: "settings" }),
    "/verbosity": () => open({ type: "settings", section: "user" }),
    "/permissions": () => open({ type: "settings", section: "user" }),
    "/mcp": () => open({ type: "settings", section: "user" }),
    "/tools": () => setModal({ type: "tools", session_id: selected }),
    "/rename": () => session && setModal({ type: "rename", session }),
    "/memory": () => showLibrary("memory"),
    "/skills": () => showLibrary("skills"),
    "/schedule": () => setPage("scheduled"),
    // The list is always beside a wide conversation: search instead.
    "/sessions": () => (compact ? setDrawer(true) : search()),
  };
  function showLibrary(kind: "memory" | "skills") {
    setLibraryKind(kind);
    setPage("library");
  }
  // A fork opens as its own conversation; the original stays as it was.
  // Editing (a rewind) also puts the message it forked before back into
  // the composer.
  async function forkAndOpen(
    target: Session | undefined,
    fields: CommandFields<"fork"> = {},
    edit = false,
  ) {
    const result = await command("fork", target, fields);
    if (result.pending) return;
    const { id, prompt, rewound } = result.result;
    if (!rewound) {
      await refresh();
      await choose(id);
      await command("activate", { id, generation: "" });
    }
    await load(id);
    if (edit && prompt) setDraft({ text: prompt, files: [] }, id);
    if (edit)
      setNotice(
        rewound
          ? "The lead agent rewound to this message; edit and send."
          : "Continuing in a fork. Files on disk are unchanged.",
      );
  }
  async function localCommand(text: string) {
    const { name, argument, clientOnly } = parseSlash(
      catalogue.commands || [],
      text,
    );
    if (!argument && screens[name]) screens[name]();
    else if (name === "/reset") await startConversation(session!.cwd!);
    else if (name === "/panel")
      await startConversation(folderOf(session!), true);
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
      await askSide(session!, argument);
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
    }
    // The runtime refuses the rest of the client's own commands.
    else if (clientOnly) throw new Error(`${name} works only in a terminal.`);
    else return false;
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
          parseSlash(catalogue.commands || [], sent.text).name === "/attach" &&
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
      kind === "steer" &&
      (queue ? outgoingStatus.next : outgoingStatus.guidance);
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
          status: queued || outgoingStatus.sending,
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
        status:
          queued ||
          (result.pending ? outgoingStatus.awaiting : outgoingStatus.sent),
      });
    } catch (error) {
      const issue = failure(error);
      if (visible)
        patchOutgoing(request_id, {
          status: issue.rejected
            ? outgoingStatus.refused
            : outgoingStatus.unconfirmed,
          error: issue.message,
        });
      else report(issue);
    } finally {
      setBusy(false);
    }
  }
  // The context a session was last sent, its system prompt first: the open
  // conversation's, or that of the one given.
  function showContext(item?: Session) {
    const shown = item ?? session;
    const busy = item ? item.turn_active : running;
    const prepare = shown?.generation && !busy && online ? shown : undefined;
    const id = shown?.id || selected;
    setModal({
      type: "raw",
      context: true,
      session: id,
      exchanges: prepare ? [] : snapshots.get()[id]?.state?.http || [],
      prepare,
    });
  }
  // A slash command from the palette runs as if sent from the composer;
  // one that needs an argument waits there for it.
  async function runCommand(entry: SlashCommand) {
    try {
      if (entry.argument && !entry.argument.startsWith("[")) {
        if (!session)
          throw new Error(`Open a conversation to use ${entry.command}`);
        setDraft({ ...draft, text: `${entry.command} ` });
        focusComposer();
      } else if (!(await localCommand(entry.command))) {
        if (!session)
          throw new Error(`Open a conversation to run ${entry.command}`);
        if (running)
          throw new Error(
            "Wait for this turn to finish before running a slash command.",
          );
        await act("submit", { request_id: requestId(), text: entry.command });
      }
    } catch (failure) {
      report(failure);
    }
  }
  return { submit, deliver, forkAndOpen, showContext, runCommand };
}
