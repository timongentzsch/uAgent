import { ImageTile } from "../../shared/attachments.tsx";
import "../../shared/attachments.css";
import { useCommandSuggestions } from "./command-suggestions.tsx";
import { useFileMentions } from "./file-mentions.tsx";
import { useExit } from "../../shared/motion.ts";
import { plainKey } from "../../shared/listbox-nav.ts";
import { parseSlash } from "./slash.ts";
import { Button, IconButton, Input, updateChecked } from "../../shared/ui.tsx";
import { bytes, plural } from "../../shared/quantities.ts";
import { holdable } from "../../shared/message-view.ts";
import type {
  InspectorTarget,
  SlashCommand,
  Session,
  State,
  Pending,
  Node,
  Outgoing,
} from "../../shared/types.ts";
import type { ComponentChildren, TargetedKeyboardEvent } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { ArrowUp, Paperclip, Square, X } from "lucide-preact";
import { emptyDraft } from "../../state/store.ts";
import { useStoreValue } from "../../state/value-store.ts";
import { useApi } from "../../state/api.ts";
import { useConversation, useHostContext } from "../../state/host-context.ts";
import { isRunning } from "../../state/attention.ts";
import { JumpToLatest } from "../../shared/jump-to-latest.tsx";
import { dedupeName } from "./mention.ts";
import ApprovalControl from "./approval-control.tsx";
import {
  AgentStack,
  AgentsButton,
  ActivityButton,
  type SideRows,
} from "../../shared/turn-status.tsx";
import MessageInput, { QuoteBar } from "../../shared/message-input.tsx";
import ModelControl from "../../shared/model-control.tsx";
import { useNarrowColumn } from "../../shared/layout.ts";
import { SessionSummary } from "../../shared/session-summary.tsx";
import Decision from "../../shared/decision.tsx";
import {
  dropOldest,
  maxRecalledPrompts,
  maxRecalledPromptSessions,
  stopGuardMs,
} from "../../shared/limits.ts";

import GuidanceQueue from "./guidance-queue.tsx";
import TodoList from "./todo-list.tsx";
import { focusComposer } from "../../shared/navigation.ts";
import "./composer.css";
// Prompts sent from this page per session, oldest first, for Up/Down recall.
const sentPrompts = new Map<string, string[]>();
const EMPTY_DRAFT = emptyDraft();
const NONE: [] = [];

function Composer({
  session,
  queued = NONE,
  recall,
  insert,
  commands,
  state,
  pending,
  submit,
  asking,
  jump,
  unseen = 0,
  openInspector,
  showStatistics,
  openBrowser,
  stopped,
  resume,
  agents,
  reports,
  sides,
  above,
  choose,
}: {
  session: Session;
  queued?: Outgoing[];
  recall?: (node: Node) => Promise<void>;
  insert?: (node: Node) => Promise<void>;
  commands: SlashCommand[];
  state?: State;
  pending?: Pending | null;
  submit: (event: Event, queue?: boolean) => void;
  // The message is a side chat's question about this, not the conversation's.
  asking?: { quote: string; cancel: () => void };
  jump: () => void;
  unseen?: number;
  openInspector: (target: InspectorTarget) => void;
  showStatistics: () => void;
  openBrowser: () => void;
  stopped?: string;
  resume?: () => void;
  agents: Session[];
  reports: Record<string, string>;
  // The conversation's side chats, for the Activity list.
  sides: SideRows;
  // What sits on the input: Jump to latest stays clear of it.
  above?: ComponentChildren;
  choose: (id: string) => void;
}) {
  const { routes } = useApi();
  const { online, connection, report } = useHostContext();
  const narrow = useNarrowColumn();
  const { drafts, setDraft, upload, busy, act, following } = useConversation();
  const draft = useStoreValue(drafts, (all) => all[session.id] ?? EMPTY_DRAFT);
  const uploading = draft.files.some((file) => file.pending);
  const input = useRef<HTMLTextAreaElement>(null);
  const recalled = useRef(-1);
  // A send empties the draft and turns Send into Stop under the same
  // finger: a second tap that soon is the first one repeated, not a stop.
  const lastSent = useRef(0);
  const [renaming, setRenaming] = useState<string | null>(null);
  const mentions = useFileMentions(
    session.id,
    draft,
    setDraft,
    input,
    online && !pending,
    !!asking,
  );
  const commitRename = (id: string, raw: string) => {
    setRenaming(null);
    const taken = draft.files
      .filter((item) => item.id !== id)
      .map((item) => item.name);
    const name = dedupeName(raw, taken);
    if (!draft.files.some((item) => item.id === id && item.name !== name))
      return;
    setDraft({
      ...draft,
      files: draft.files.map((item) =>
        item.id === id ? { ...item, name } : item,
      ),
    });
  };
  const suggestions = useCommandSuggestions(
    commands,
    draft.text,
    (text) => setDraft({ ...draft, text }),
    input,
  );
  const suggestionCount = mentions.count || suggestions.count;
  const send = (event: Event, queue = false) => {
    if (pending) {
      event.preventDefault();
      return;
    }
    const slash = parseSlash(commands, draft.text);
    if (slash.name === "/attach" && !slash.argument) {
      event.preventDefault();
      input.current?.form
        ?.querySelector<HTMLInputElement>("input[type=file]")
        ?.click();
      setDraft({ ...draft, text: "" });
    } else {
      if (draft.text.trim()) {
        const past = sentPrompts.get(session.id) || [];
        if (past.at(-1) !== draft.text) {
          // Re-insert so Map order is recency; the oldest session drops out.
          sentPrompts.delete(session.id);
          sentPrompts.set(
            session.id,
            [...past, draft.text].slice(-maxRecalledPrompts),
          );
          dropOldest(sentPrompts, maxRecalledPromptSessions);
        }
      }
      recalled.current = -1;
      lastSent.current = performance.now();
      submit(event, queue);
    }
  };
  // Recall only from an empty draft or the entry being browsed, so arrows
  // still move the caret through text the person is writing.
  const recallKeyDown = (event: TargetedKeyboardEvent<HTMLTextAreaElement>) => {
    const past = sentPrompts.get(session.id) || [];
    const at = recalled.current;
    const browsing = at >= 0 && draft.text === past[at];
    if (!browsing) recalled.current = -1;
    let next: number;
    if (event.key === "ArrowUp" && (browsing || !draft.text) && past.length)
      next = browsing ? Math.max(0, at - 1) : past.length - 1;
    else if (event.key === "ArrowDown" && browsing)
      next = at + 1 < past.length ? at + 1 : -1;
    else return false;
    event.preventDefault();
    recalled.current = next;
    setDraft({ ...draft, text: next < 0 ? "" : past[next] });
    return true;
  };
  // Focus left with the decision card returns to the input below it.
  const decided = useRef(!!pending);
  useEffect(() => {
    if (!pending && decided.current && document.activeElement === document.body)
      focusComposer();
    decided.current = !!pending;
  }, [pending]);
  const running = isRunning(session, online);
  const [jumpShown, jumpRef] = useExit<boolean, HTMLButtonElement>(
    !following && !pending,
  );
  // Asking a side chat, the draft's quotes wait for the conversation.
  const empty =
    !draft.text.trim() &&
    !draft.files.length &&
    (!!asking || !draft.quotes.length);
  const stopping = running && empty;
  const unsendable =
    (!online && !holdable(draft)) || busy || uploading || empty || !!pending;
  const stopLabel = !stopped
    ? ""
    : stopped === "cancelled"
      ? "Stopped"
      : stopped === "error"
        ? "Stopped by an error"
        : `Stopped: ${stopped.replaceAll("_", " ")}`;
  const queueing = running && !pending && !empty;
  const continuing = !running && !pending && !!stopLabel && !!resume;
  // What is asked about is asked at once.
  const side = !!asking;
  useEffect(() => {
    if (side) input.current?.focus({ preventScroll: true });
  }, [side]);
  return (
    <section class="composer">
      {jumpShown && (
        <JumpToLatest buttonRef={jumpRef} unseen={unseen} onClick={jump} />
      )}
      {above}
      {pending?.kind === "browser" ? (
        <section class="card decision enters" aria-label="Browser needs you">
          <h2>Continue in the browser</h2>
          <p>{pending.prompt || "The agent needs you to finish in Chrome."}</p>
          <Button variant="primary" disabled={!online} onClick={openBrowser}>
            Open browser
          </Button>
        </section>
      ) : (
        pending && (
          <Decision
            key={pending.id}
            pending={pending}
            session={session.id}
            cwd={session.cwd}
            act={act}
            online={online}
            report={report}
          />
        )
      )}
      <TodoList items={state?.todo} />
      <GuidanceQueue
        items={queued}
        online={online}
        held={!!pending}
        recall={recall}
        insert={insert}
      />
      <AgentStack
        agents={agents}
        reports={reports}
        online={online}
        report={report}
        choose={choose}
      />
      <form class="message-box" onSubmit={send}>
        {suggestions.list}
        {mentions.list}
        {asking ? (
          <QuoteBar
            quote={asking.quote}
            remove={asking.cancel}
            label="Ask in the conversation instead"
          >
            <span class="chip">Side chat</span>
          </QuoteBar>
        ) : (
          draft.quotes.map((quote, at) => (
            <QuoteBar
              key={at}
              quote={quote}
              remove={() =>
                setDraft({
                  ...draft,
                  quotes: draft.quotes.filter((_, other) => other !== at),
                })
              }
            />
          ))
        )}
        <label class="sr-only" for="prompt">
          Message or guidance
        </label>
        <span class="sr-only" role="status">
          {suggestionCount > 0 && plural(suggestionCount, "suggestion")}
        </span>
        <MessageInput
          submit={send}
          {...suggestions.attributes}
          {...mentions.attributes}
          onBlur={() => {
            suggestions.attributes.onBlur();
            mentions.blur();
          }}
          id="prompt"
          inputRef={input}
          rows={1}
          readOnly={!!pending}
          placeholder={
            pending
              ? "Decide above to continue"
              : asking
                ? "Ask about it…"
                : running
                  ? "Add guidance… (Esc to stop)"
                  : "Ask µAgent…"
          }
          value={draft.text}
          onInput={(event) => {
            setDraft({ ...draft, text: event.currentTarget.value });
            mentions.changed(event.currentTarget.selectionStart ?? 0);
          }}
          onKeyUp={(event) =>
            mentions.at(event.currentTarget.selectionStart ?? 0)
          }
          onClick={(event) =>
            mentions.changed(event.currentTarget.selectionStart ?? 0)
          }
          onPaste={(event) => {
            const files = [...(event.clipboardData?.files || [])];
            // Spreadsheets and web pages put a rendered image beside the
            // text; the text is what was copied.
            if (files.length && !event.clipboardData?.getData("text/plain")) {
              event.preventDefault();
              upload(files);
            }
          }}
          onKeyDown={(event) => {
            if (mentions.keyDown(event)) return;
            if (queueing && event.key === "Enter" && event.altKey) {
              event.preventDefault();
              if (!event.repeat) send(event, true);
              return;
            }
            if (suggestions.keyDown(event) || !plainKey(event)) return;
            if (event.key === "Escape" && running) {
              event.preventDefault();
              act("interrupt").catch(report);
            } else recallKeyDown(event);
          }}
        />
        {!!state?.attachments && (
          <small class="muted host-files">
            {state.attachments} file(s) attached on the host
            <Button
              variant="quiet"
              size="compact"
              disabled={!online}
              onClick={() =>
                act("submit", { text: "/attach clear" }).catch(report)
              }
            >
              Remove
            </Button>
          </small>
        )}
        {draft.files.length > 0 && (
          <div class="attachments">
            {draft.files.map((asset) => (
              <div
                class="chip chip-outline file-chip"
                key={asset.id}
                aria-busy={asset.pending || undefined}
              >
                {asset.image && !asset.pending ? (
                  <ImageTile
                    draftId={asset.id}
                    name={asset.name}
                    src={routes.asset(session.id, asset.id)}
                  />
                ) : (
                  <Paperclip />
                )}
                <span title={asset.name}>
                  {renaming === asset.id && !asset.pending ? (
                    <Input
                      aria-label={`Rename ${asset.name}`}
                      defaultValue={asset.name}
                      autoFocus
                      onKeyDown={(event) => {
                        if (event.key === "Enter")
                          commitRename(asset.id, event.currentTarget.value);
                        else if (event.key === "Escape") {
                          // Reset before closing: the unmount blur
                          // would otherwise commit the edit.
                          event.currentTarget.value = asset.name;
                          setRenaming(null);
                        }
                      }}
                      onBlur={(event) =>
                        commitRename(asset.id, event.currentTarget.value)
                      }
                    />
                  ) : (
                    <Button
                      class="chip-name quiet"
                      title={`Rename ${asset.name}`}
                      aria-label={`Rename ${asset.name}`}
                      disabled={asset.pending}
                      onClick={() => setRenaming(asset.id)}
                    >
                      {asset.name}
                    </Button>
                  )}
                  <small>
                    {asset.pending ? "Uploading…" : bytes(asset.bytes)}
                  </small>
                </span>
                {!asset.pending && (
                  <IconButton
                    label={`Remove ${asset.name}`}
                    onClick={() =>
                      setDraft({
                        ...draft,
                        files: draft.files.filter(
                          (item) => item.id !== asset.id,
                        ),
                      })
                    }
                  >
                    <X />
                  </IconButton>
                )}
              </div>
            ))}
          </div>
        )}
        {/* raw: the draft's own row, which joins the draft's line only above a width. */}
        <div
          class="composer-actions message-actions"
          data-held={!!pending || undefined}
        >
          <label class="file-button icon-button" title="Attach files">
            <Paperclip />
            <Input
              type="file"
              aria-label="Attach files"
              multiple
              disabled={!online || uploading || !!pending}
              onChange={(event) => {
                upload([...(event.currentTarget.files || [])]);
                event.currentTarget.value = "";
              }}
            />
          </label>
          <ModelControl
            key={session.id}
            session={session}
            state={state}
            online={online}
            running={running || !!pending}
          />
          <ApprovalControl
            session={session}
            permission={state?.permissions}
            online={online}
            report={report}
          />
          <IconButton
            class="composer-primary"
            data-mode={stopping ? "stop" : "send"}
            type={stopping ? "button" : "submit"}
            variant="primary"
            label={stopping ? "Stop" : running ? "Send guidance" : "Send"}
            title={stopping ? "Stop (Esc)" : running ? "Send guidance" : "Send"}
            disabled={stopping ? !online || !!pending : unsendable}
            onClick={
              stopping
                ? () => {
                    if (performance.now() - lastSent.current < stopGuardMs)
                      return;
                    act("interrupt").catch(report);
                  }
                : undefined
            }
          >
            <ArrowUp class="send-icon" aria-hidden="true" />
            <Square class="stop-icon" aria-hidden="true" />
          </IconButton>
        </div>
      </form>
      <div class="composer-metrics">
        <div class="status-line">
          <ActivityButton
            open={openInspector}
            session={session}
            online={online}
            connection={connection}
            report={report}
            items={state?.activities || NONE}
            sides={sides}
            agents={agents}
            running={running}
            started={state?.turn_started_ms}
            pending={!!pending}
            stopped={continuing ? stopLabel : undefined}
            resume={resume}
          />
          <AgentsButton
            agents={agents}
            reports={reports}
            online={online}
            report={report}
            choose={choose}
          />
          {running && (
            <Button
              variant="quiet"
              size="compact"
              class="queue-next"
              data-shown={queueing}
              aria-label="Queue next"
              aria-keyshortcuts="Alt+Enter"
              title="Send when this turn ends (Alt+Enter)"
              disabled={!queueing || !online || busy || uploading}
              onClick={(event) => send(event, true)}
            >
              Queue next{" "}
              <kbd class="muted" aria-hidden="true">
                Alt+Enter
              </kbd>
            </Button>
          )}
        </div>
        {!narrow && (
          <div class="metrics small">
            <SessionSummary
              state={state}
              queued={queued.length ? 0 : session.guidance}
              open={showStatistics}
            />
          </div>
        )}
      </div>
    </section>
  );
}

// The page renders with every streamed block; the composer only when what
// it shows changes.
export default updateChecked(Composer);
