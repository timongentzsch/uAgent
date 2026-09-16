import "./attachments.css";
import { useCommandSuggestions } from "./command-suggestions.tsx";
import { parseSlash } from "./slash.ts";
import { Deferred, Field, Select } from "./ui.tsx";
import { DecisionSkeleton, ModelSkeleton } from "./loading.tsx";
import { bytes, count } from "./quantities.ts";
import { contextSummary } from "./context.ts";
import { observeResize } from "./layout.ts";
import type {
  SlashCommand,
  Session,
  Snapshot,
  Draft,
  Act,
  Report,
  Block,
  Sizes,
} from "./types.ts";
import type { JSX } from "preact";
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import {
  ArrowDown,
  ArrowUp,
  ChevronDown,
  Gauge,
  Paperclip,
  Shield,
  Square,
  X,
} from "lucide-preact";
import { command } from "./api.ts";
import { dedupeName, encodeMention, matchMention } from "./mention.ts";
import { Popover } from "./popover.tsx";
import Activities from "./activity-status.tsx";
const modelPicker = () => import("./model-picker.tsx");
const decisionPanel = () => import("./decision.tsx");

export default function Composer({
  session,
  commands,
  snapshot,
  online,
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
  activityTarget,
  clearActivity,
  showContext,
  showStatistics,
  sizes,
}: {
  session: Session;
  commands: SlashCommand[];
  snapshot?: Snapshot;
  online: boolean;
  draft: Draft;
  setDraft: (draft: Draft) => void;
  upload: (files: File[]) => void;
  uploading: boolean;
  busy: boolean;
  submit: (event: Event) => void;
  act: Act;
  report: Report;
  following: boolean;
  jump: () => void;
  activityTarget: Block | null;
  clearActivity: () => void;
  showContext: () => void;
  showStatistics: () => void;
  sizes: Sizes;
}) {
  const input = useRef<HTMLTextAreaElement>(null);
  const [renaming, setRenaming] = useState<string | null>(null);
  // @-mention over attached files: caret-driven, independent of the
  // slash menu (slash only matches a lone leading /command).
  const [caret, setCaret] = useState(0);
  const [mentionIndex, setMentionIndex] = useState(-1);
  const [mentionClosed, setMentionClosed] = useState(false);
  const mention = matchMention(draft.text, caret);
  const mentionCandidates = mention
    ? draft.files.filter(
        (item) =>
          !item.pending &&
          item.name.toLowerCase().includes(mention.query.toLowerCase()),
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
    if (
      event.isComposing ||
      event.shiftKey ||
      event.ctrlKey ||
      event.metaKey ||
      event.altKey
    )
      return false;
    if (event.key === "Escape") setMentionClosed(true);
    else if (event.key === "ArrowDown" || event.key === "ArrowUp")
      setMentionIndex(
        (mentionIndex +
          (event.key === "ArrowDown" ? 1 : mentionIndex < 0 ? 0 : -1) +
          mentionCandidates.length) %
          mentionCandidates.length,
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
  const send = (event: Event) => {
    const slash = parseSlash(commands, draft.text);
    if (slash.name === "/attach" && !slash.argument) {
      event.preventDefault();
      input.current?.form
        ?.querySelector<HTMLInputElement>("input[type=file]")
        ?.click();
      setDraft({ ...draft, text: "" });
    } else submit(event);
  };
  const pending = online ? snapshot?.pending : null;
  const running = online && !!session.turn_active;
  const state = snapshot?.state;
  // A session without a live worker names its lifecycle state here instead
  // of a phase, so closing a session still reports that it was saved.
  const detached = ["saved", "closed", "draft", "starting"].includes(
    session?.status || "",
  )
    ? (session?.status || "").charAt(0).toUpperCase() +
      (session?.status || "").slice(1)
    : "";
  useLayoutEffect(() => {
    const element = input.current;
    if (!element) return;
    let lastWidth = element.clientWidth;
    const resize = () => {
      lastWidth = element.clientWidth;
      const next = element.scrollHeight;
      // Guard the write: an identical height must not touch layout, or
      // the transcript box observer pins to a no-op resize while
      // content-visibility re-estimates rows above (the 1→2 line wrap
      // glitch that read as history jumping while typing).
      if (Math.abs(next - element.clientHeight) < 1 && element.style.height)
        return;
      element.style.height = "0px";
      element.style.height = `${element.scrollHeight}px`;
    };
    resize();
    // Width-only parent subscription: the textarea's own height growth
    // changes the parent height, which must not re-trigger a shrink to
    // zero (the old feedback loop). Only a width change alters wrapping.
    const onParent = () => {
      if (element.clientWidth !== lastWidth) resize();
    };
    return observeResize(onParent, element.parentElement!);
  }, [draft.text, sizes.text, sizes.display, pending?.id, session.generation]);
  const permission = state?.permissions;
  const effective =
    permission?.mode === "default" ? permission.default : permission?.mode;
  const permissionLabel = effective === "yolo" ? "YOLO" : "Ask";
  return (
    <section
      class="composer"
      onDragOver={(event) => event.preventDefault()}
      onDrop={(event) => {
        event.preventDefault();
        upload([...(event.dataTransfer?.files || [])]);
      }}
    >
      {!following && !pending && (
        <button class="jump quiet with-icon" onClick={jump}>
          Jump to latest <ArrowDown />
        </button>
      )}
      {pending ? (
        <Deferred
          load={decisionPanel}
          key={pending.id}
          pending={pending}
          act={act}
          online={online}
          report={report}
          fallback={<DecisionSkeleton />}
        />
      ) : online && !session.generation ? (
        <button
          class="primary"
          disabled={!online}
          onClick={() => command("activate", session).catch(report)}
        >
          Resume in this host directory
        </button>
      ) : (
        <form onSubmit={send}>
          {suggestions.list}
          {mentionOpen && (
            <div
              class="command-suggestions"
              role="listbox"
              aria-label="Attached files"
            >
              {mentionCandidates.map((item, position) => (
                <button
                  key={item.id}
                  type="button"
                  role="option"
                  aria-selected={position === mentionIndex}
                  tabIndex={-1}
                  onMouseDown={(event) => event.preventDefault()}
                  onClick={() => insertMention(item.id)}
                >
                  <strong>@{item.name}</strong>
                  <span>{bytes(item.bytes)}</span>
                </button>
              ))}
            </div>
          )}
          <label class="sr-only" for="prompt">
            Message or guidance
          </label>
          <textarea
            {...suggestions.attributes}
            id="prompt"
            ref={input}
            rows={1}
            placeholder={running ? "Add guidance…" : "Ask µAgent…"}
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
              if (files.length) {
                event.preventDefault();
                upload(files);
              }
            }}
            onKeyDown={(event) => {
              if (mentionOpen && mentionKeyDown(event)) return;
              if (suggestions.keyDown(event)) return;
              if (
                event.key === "Enter" &&
                !event.shiftKey &&
                !event.isComposing &&
                event.keyCode !== 229
              ) {
                event.preventDefault();
                if (!event.repeat) send(event);
              }
            }}
          />
          {!!state?.attachments && (
            <small class="muted">
              {state.attachments} file(s) attached on the host · /attach clear
              to remove
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
                    <img
                      alt={asset.name}
                      src={`/api/sessions/${session.id}/assets/${asset.id}`}
                    />
                  ) : (
                    <Paperclip />
                  )}
                  <span title={asset.name}>
                    {renaming === asset.id && !asset.pending ? (
                      <input
                        aria-label={`Rename ${asset.name}`}
                        defaultValue={asset.name}
                        // eslint-disable-next-line jsx-a11y/no-autofocus
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
                      <button
                        type="button"
                        class="chip-name"
                        title={`Rename ${asset.name}`}
                        aria-label={`Rename ${asset.name}`}
                        disabled={asset.pending}
                        onClick={() => setRenaming(asset.id)}
                      >
                        {asset.name}
                      </button>
                    )}
                    <small>
                      {asset.pending ? "Uploading…" : bytes(asset.bytes)}
                    </small>
                  </span>
                  {!asset.pending && (
                    <button
                      type="button"
                      class="quiet icon-button"
                      aria-label={`Remove ${asset.name}`}
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
                    </button>
                  )}
                </div>
              ))}
            </div>
          )}
          <div class="composer-actions">
            <label class="file-button icon-button" title="Attach files">
              <Paperclip />
              <input
                type="file"
                aria-label="Attach files"
                multiple
                disabled={!online || uploading}
                onChange={(event) => {
                  upload([...(event.currentTarget.files || [])]);
                  event.currentTarget.value = "";
                }}
              />
            </label>
            <Popover
              key={session.id}
              label="Model and effort"
              title={state?.route || "Select model"}
              side="top"
              align="start"
              className="model-control"
              buttonClass="quiet model-selector with-icon"
              disabled={!online || running}
              trigger={
                <>
                  <Gauge />
                  <span>{state?.route || "Select model"}</span>
                  <ChevronDown />
                </>
              }
            >
              {(close) => (
                <Deferred
                  load={modelPicker}
                  session={session}
                  state={state}
                  online={online}
                  running={running}
                  close={close}
                  fallback={
                    <div class="model-form">
                      <ModelSkeleton />
                      <div class="dialog-actions">
                        <button type="button" onClick={close}>
                          Cancel
                        </button>
                        <button type="button" class="primary" disabled>
                          Apply
                        </button>
                      </div>
                    </div>
                  }
                />
              )}
            </Popover>
            <Popover
              label="Permissions"
              title={`${permissionLabel}${permission?.mode === "default" ? " · using default permissions" : " · conversation override"}`}
              className="permission-control"
              panelClass="permission-panel"
              side="top"
              buttonClass="quiet with-icon"
              disabled={!online}
              trigger={
                <>
                  <Shield />
                  <span>{permissionLabel}</span>
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
                      {permission?.default === "yolo" ? "YOLO" : "Ask"}
                    </option>
                    <option value="ask">Ask</option>
                    <option value="yolo">YOLO</option>
                  </Select>
                </Field>
              )}
            </Popover>
            {running && (
              <button
                type="button"
                class="quiet icon-button"
                aria-label="Stop"
                title="Stop"
                disabled={!online}
                onClick={() => act("interrupt").catch(report)}
              >
                <Square />
              </button>
            )}
            <button
              class="primary icon-button"
              aria-label={running ? "Send guidance" : "Send"}
              title={running ? "Send guidance" : "Send"}
              disabled={
                !online ||
                busy ||
                uploading ||
                (!draft.text.trim() && !draft.files.length)
              }
            >
              <ArrowUp />
            </button>
          </div>
        </form>
      )}
      <Activities
        collaborators={state?.collaborators || []}
        target={activityTarget}
        clearTarget={clearActivity}
        cwd={session.cwd || ""}
        session={session}
        online={online}
        report={report}
        items={online ? state?.activities || [] : []}
        present={online && !!session?.presence}
        phase={
          !online
            ? "Disconnected"
            : detached || state?.activity || (state ? "Ready" : "Loading…")
        }
        running={online && running}
        pending={online ? pending : null}
      >
        <div class="metrics">
          <button
            class="quiet"
            aria-label="Raw context"
            title={`${state?.context_tokens?.toLocaleString() || "—"}${state?.context_window ? ` / ${state.context_window.toLocaleString()}` : ""} tokens · View raw context`}
            disabled={!online}
            onClick={showContext}
          >
            {contextSummary(state?.context_tokens, state?.context_window)}
          </button>
          <button class="quiet" onClick={showStatistics}>
            Session · {count(state?.statistics?.recorded_turns ?? state?.turns)}{" "}
            turns
            {state?.usage?.cost_reported
              ? ` · $${state.usage.cost.toFixed(4)}`
              : ""}
          </button>
          {!!session.guidance && (
            <span class="muted" role="status">
              {session.guidance} guidance queued
            </span>
          )}
        </div>
      </Activities>
    </section>
  );
}
