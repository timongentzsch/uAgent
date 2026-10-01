import type { ConnectionPhase } from "../../shared/connection-status.tsx";
import { ImageTile } from "../../shared/attachments.tsx";
import "./attachments.css";
import {
  SuggestionList,
  useCommandSuggestions,
} from "./command-suggestions.tsx";
import { nextIndex, plainKey } from "../../shared/listbox-nav.ts";
import { parseSlash } from "./slash.ts";
import {
  Button,
  Deferred,
  Field,
  IconButton,
  Select,
  Input,
  Spinner,
  DataText,
} from "../../shared/ui.tsx";
import { bytes, plural } from "../../shared/quantities.ts";
import type {
  SlashCommand,
  Session,
  Snapshot,
  Draft,
  Act,
  Report,
  SessionStatus,
} from "../../shared/types.ts";
import type { JSX } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { ArrowUp, Paperclip, Shield, Square, X } from "lucide-preact";
import { command } from "../../state/api.ts";
import { JumpToLatest } from "../../shared/jump-to-latest.tsx";
import {
  dedupeName,
  encodeMention,
  matchMention,
  mentionOptions,
} from "./mention.ts";
import { SheetButton } from "../../shared/sheet.tsx";
import { ActivityButton, ActivityStatus } from "../chat/activity-status.tsx";
import type { InspectorTarget } from "../chat/inspector.tsx";
import MessageInput from "./message-input.tsx";
import ModelControl from "./model-control.tsx";
import { ContextSummary, SessionSummary } from "../chat/session-summary.tsx";
const decisionPanel = () => import("../chat/decision.tsx");
import { maxRecalledPromptSessions } from "../../shared/limits.ts";
import { permissionLabels } from "../../shared/display.ts";
// Prompts sent from this page per session, oldest first: Up and Down recall
// them the way the terminal composer does.
const sentPrompts = new Map<string, string[]>();

export default function Composer({
  session,
  commands,
  snapshot,
  online,
  connection,
  draft,
  setDraft,
  upload,
  uploading,
  busy,
  submit,
  act,
  report,
  following,
  jump,
  unseen = 0,
  openInspector,
  showContext,
  showStatistics,
  openBrowser,
  zoom,
  stopped,
  resume,
}: {
  session: Session;
  commands: SlashCommand[];
  snapshot?: Snapshot;
  online: boolean;
  connection: ConnectionPhase;
  draft: Draft;
  setDraft: (draft: Draft) => void;
  upload: (files: File[]) => void;
  uploading: boolean;
  busy: boolean;
  submit: (event: Event, queue?: boolean) => void;
  act: Act;
  report: Report;
  following: boolean;
  jump: () => void;
  unseen?: number;
  openInspector: (target: InspectorTarget) => void;
  showContext: () => void;
  showStatistics: () => void;
  openBrowser: () => void;
  zoom: number;
  // Why the last turn stopped short, while nothing has been sent since.
  stopped?: string;
  // Continues that stopped turn.
  resume?: () => void;
}) {
  const input = useRef<HTMLTextAreaElement>(null);
  const recalled = useRef(-1);
  // A send empties the draft and turns Send into Stop under the same
  // finger: a second tap that soon is the first one repeated, not a stop.
  const lastSent = useRef(0);
  const [renaming, setRenaming] = useState<string | null>(null);
  // @-mention over attached files: caret-driven, independent of the
  // slash menu (slash only matches a lone leading /command).
  const [caret, setCaret] = useState(0);
  const [mentionIndex, setMentionIndex] = useState(-1);
  const [mentionClosed, setMentionClosed] = useState(false);
  const mention = matchMention(draft.text, caret);
  const mentionCandidates = mention
    ? mentionOptions(
        draft.files.filter((item) => !item.pending),
        mention.query,
      )
    : [];
  const mentionOpen =
    !!mention && !mentionClosed && mentionCandidates.length > 0;
  const insertMention = (id: string) => {
    const target = draft.files.find((item) => item.id === id);
    if (!mention || !target || !input.current) return;
    const token = `${encodeMention(target.name, target.id)} `;
    setDraft({
      ...draft,
      text:
        draft.text.slice(0, mention.start) +
        token +
        draft.text.slice(mention.end),
    });
    setMentionIndex(-1);
    setMentionClosed(true);
    const position = mention.start + token.length;
    // The textarea value commits on the next render; place the caret after.
    requestAnimationFrame(() => {
      input.current?.setSelectionRange(position, position);
      input.current?.focus({ preventScroll: true });
    });
  };
  const mentionKeyDown = (
    event: JSX.TargetedKeyboardEvent<HTMLTextAreaElement>,
  ) => {
    if (!plainKey(event)) return false;
    if (event.key === "Escape") setMentionClosed(true);
    else if (event.key === "ArrowDown" || event.key === "ArrowUp")
      setMentionIndex(
        nextIndex(mentionIndex, event.key, mentionCandidates.length),
      );
    else if (
      event.key === "Tab" ||
      (event.key === "Enter" && mentionIndex >= 0)
    ) {
      const target =
        mentionCandidates[
          Math.min(mentionIndex, mentionCandidates.length - 1)
        ] ??
        (mentionCandidates.length === 1 ? mentionCandidates[0] : undefined);
      if (target && !event.repeat) insertMention(target.id);
      else return false;
    } else return false;
    event.preventDefault();
    return true;
  };
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
  const suggestionCount = mentionOpen
    ? mentionCandidates.length
    : suggestions.count;
  const send = (event: Event, queue = false) => {
    // A pending decision holds the draft until it is answered.
    if (snapshot?.pending) {
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
          sentPrompts.set(session.id, [...past, draft.text].slice(-100));
          if (sentPrompts.size > maxRecalledPromptSessions)
            sentPrompts.delete(sentPrompts.keys().next().value!);
        }
      }
      recalled.current = -1;
      lastSent.current = performance.now();
      submit(event, queue);
    }
  };
  // Recall only from an empty draft or the entry being browsed, so arrows
  // still move the caret through text the person is writing.
  const recallKeyDown = (
    event: JSX.TargetedKeyboardEvent<HTMLTextAreaElement>,
  ) => {
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
  // Reconnecting keeps the last known request and activities on screen,
  // inert (their controls follow `online`), so resuming never reflows.
  const pending = snapshot?.pending;
  // Focus left with the decision card returns to the input below it.
  const decided = useRef(!!pending);
  useEffect(() => {
    if (!pending && decided.current && document.activeElement === document.body)
      document.getElementById("prompt")?.focus({ preventScroll: true });
    decided.current = !!pending;
  }, [pending]);
  const running = online && !!session.turn_active;
  const state = snapshot?.state;
  // A session without a live worker names its lifecycle state here instead
  // of a phase, so closing a session still reports that it was saved.
  const detached = (
    ["saved", "interrupted", "draft", "starting"] as SessionStatus[]
  ).includes(session?.status!)
    ? (session?.status || "").charAt(0).toUpperCase() +
      (session?.status || "").slice(1)
    : "";
  const empty = !draft.text.trim() && !draft.files.length;
  // The one primary control: Stop while a turn runs and there is nothing
  // to send, Send otherwise. Its icons cross-fade in place.
  const stopping = running && empty;
  const unsendable = !online || busy || uploading || empty || !!pending;
  const stopLabel = !stopped
    ? ""
    : stopped === "cancelled"
      ? "Stopped"
      : stopped === "error"
        ? "Stopped by an error"
        : `Stopped: ${stopped.replaceAll("_", " ")}`;
  // The status line's one action slot: Queue next while a turn runs with
  // a draft, Continue after a stop. It shows by opacity, never by reflow.
  const queueing = running && !pending && !empty;
  const continuing = !running && !pending && !!stopLabel && !!resume;
  const permission = state?.permissions;
  const effective =
    permission?.mode === "default" ? permission.default : permission?.mode;
  const permissionLabel =
    permissionLabels[
      effective === "yolo" || effective === "auto" ? effective : "ask"
    ];
  return (
    <section class="composer">
      {!following && !pending && (
        <JumpToLatest unseen={unseen} onClick={jump} />
      )}
      {pending?.kind === "browser" ? (
        <section class="decision" aria-label="Browser needs you">
          <h2>Continue in the browser</h2>
          <p>{pending.prompt || "The agent needs you to finish in Chrome."}</p>
          <Button variant="primary" disabled={!online} onClick={openBrowser}>
            Open browser
          </Button>
        </section>
      ) : (
        pending && (
          <Deferred
            load={decisionPanel}
            key={pending.id}
            pending={pending}
            session={session.id}
            cwd={session.cwd}
            act={act}
            online={online}
            report={report}
            fallback={
              <section class="decision">
                <Spinner label="Loading decision…" surface />
              </section>
            }
          />
        )
      )}
      {/* The state above the input: counts live on ActivityButton below it. */}
      <div class="activities">
        <div class="status-line">
          <span class="activity-toggle">
            <ActivityStatus
              phase={
                detached || state?.activity || (state ? "Ready" : "Loading…")
              }
              running={running}
              pending={pending}
              stopped={continuing ? stopLabel : undefined}
              present={online && !!session?.presence}
              connection={connection}
              announce
            />
          </span>
          {(running || continuing) && (
            <span class="status-action">
              {continuing ? (
                <Button
                  variant="quiet"
                  size="compact"
                  disabled={!online}
                  onClick={resume}
                >
                  Continue
                </Button>
              ) : (
                <Button
                  variant="quiet"
                  size="compact"
                  data-shown={queueing}
                  aria-label="Queue next"
                  aria-keyshortcuts="Alt+Enter"
                  title="Send when this turn ends (Alt+Enter)"
                  disabled={!queueing || !online || busy || uploading}
                  onClick={(event) => send(event, true)}
                >
                  Queue next <kbd aria-hidden="true">Alt+Enter</kbd>
                </Button>
              )}
            </span>
          )}
        </div>
      </div>
      {!session.generation ? (
        <Button
          variant="primary"
          disabled={!online}
          onClick={() => command("activate", session).catch(report)}
        >
          Resume in this host directory
        </Button>
      ) : (
        <form onSubmit={send}>
          {suggestions.list}
          {mentionOpen && (
            <SuggestionList
              id="mention-suggestions"
              label="Attached files"
              prefix="mention"
              items={mentionCandidates}
              index={mentionIndex}
              pick={(item) => insertMention(item.id)}
              keyOf={(item) => item.id}
            >
              {(item) => (
                <>
                  <strong>@{item.name}</strong>
                  <span>{bytes(item.bytes)}</span>
                </>
              )}
            </SuggestionList>
          )}
          <label class="sr-only" for="prompt">
            Message or guidance
          </label>
          <span class="sr-only" role="status">
            {suggestionCount > 0 && plural(suggestionCount, "suggestion")}
          </span>
          <MessageInput
            submit={send}
            resizeKey={zoom}
            {...suggestions.attributes}
            {...(mentionOpen && {
              "aria-expanded": true,
              "aria-controls": "mention-suggestions",
              "aria-activedescendant":
                mentionIndex >= 0 ? `mention-${mentionIndex}` : undefined,
            })}
            id="prompt"
            inputRef={input}
            rows={1}
            readOnly={!!pending}
            placeholder={
              pending
                ? "Decide above to continue"
                : running
                  ? "Add guidance… (Esc to stop)"
                  : "Ask µAgent…"
            }
            value={draft.text}
            onInput={(event) => {
              setDraft({ ...draft, text: event.currentTarget.value });
              setCaret(event.currentTarget.selectionStart ?? 0);
              setMentionIndex(-1);
              setMentionClosed(false);
            }}
            onKeyUp={(event) =>
              setCaret(event.currentTarget.selectionStart ?? 0)
            }
            onClick={(event) =>
              setCaret(event.currentTarget.selectionStart ?? 0)
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
              if (mentionOpen && mentionKeyDown(event)) return;
              // Alt+Enter holds the message until the turn ends.
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
                  class="file-chip"
                  key={asset.id}
                  aria-busy={asset.pending || undefined}
                >
                  {asset.image && !asset.pending ? (
                    <ImageTile
                      draftId={asset.id}
                      name={asset.name}
                      src={`/api/sessions/${session.id}/assets/${asset.id}`}
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
                        class="chip-name"
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
          {/* Fixed slots: Attach, Model (the only one that flexes),
              Permissions, then the primary. A pending decision dims all but
              Permissions, which can still settle it for the conversation. */}
          <div class="composer-actions" data-held={!!pending || undefined}>
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
            <SheetButton
              label="Permissions"
              title={`${permissionLabel}${permission?.mode === "default" ? " · using default permissions" : " · conversation override"}`}
              // YOLO runs everything unasked: it says so loudly.
              className={`permission-control${effective === "yolo" ? " yolo" : ""}`}
              disabled={!online}
              trigger={
                <>
                  <Shield />
                  <span>
                    <DataText>{permissionLabel}</DataText>
                  </span>
                </>
              }
            >
              {(close) => (
                <Field label="Conversation permissions">
                  <Select
                    aria-label="Permissions"
                    value={permission?.mode || "default"}
                    onChange={(event) =>
                      command("permissions", session, {
                        mode: event.currentTarget.value,
                      })
                        .then(close)
                        .catch(report)
                    }
                  >
                    <option value="default">
                      Default ·{" "}
                      {
                        permissionLabels[
                          permission?.default === "yolo" ||
                          permission?.default === "auto"
                            ? permission.default
                            : "ask"
                        ]
                      }
                    </option>
                    {Object.entries(permissionLabels).map(([value, label]) => (
                      <option value={value}>{label}</option>
                    ))}
                  </Select>
                </Field>
              )}
            </SheetButton>
            <IconButton
              class="composer-primary"
              data-mode={stopping ? "stop" : "send"}
              type={stopping ? "button" : "submit"}
              variant="primary"
              label={stopping ? "Stop" : running ? "Send guidance" : "Send"}
              title={
                stopping ? "Stop (Esc)" : running ? "Send guidance" : "Send"
              }
              disabled={stopping ? !online || !!pending : unsendable}
              onClick={
                stopping
                  ? () => {
                      if (performance.now() - lastSent.current < 500) return;
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
      )}
      <div class="composer-metrics">
        <ActivityButton
          agents={state?.agents || []}
          open={openInspector}
          session={session}
          online={online}
          report={report}
          items={state?.activities || []}
        />
        <div class="metrics">
          <ContextSummary state={state} open={showContext} online={online} />
          <SessionSummary state={state} open={showStatistics} />
          {!!session.guidance && (
            <span class="muted" role="status">
              {session.guidance} guidance queued
            </span>
          )}
        </div>
      </div>
    </section>
  );
}
