import "./attachments.css";
import { ModelSkeleton } from "./loading.tsx";
import { bytes } from "./quantities.ts";
import { contextSummary } from "./context.ts";
import { observeResize } from "./layout.ts";
import type {
  Session,
  Snapshot,
  Draft,
  Act,
  Report,
  Block,
  Sizes,
} from "./types.ts";
import { useLayoutEffect, useRef } from "preact/hooks";
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
import { command } from "./store.ts";
import { Deferred, Field, Select, Skeleton } from "./ui.tsx";
import { Popover } from "./popover.tsx";
import Activities from "./activity-status.tsx";
const modelPicker = () => import("./model-picker.tsx");
const decisionPanel = () => import("./decision.tsx");

export default function Composer({
  session,
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
  sizes,
}: {
  session: Session;
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
  sizes: Sizes;
}) {
  const input = useRef<HTMLTextAreaElement>(null);
  const pending = snapshot?.pending;
  const running = !!session.turn_active;
  const state = snapshot?.state;
  useLayoutEffect(() => {
    const element = input.current;
    if (!element) return;
    const resize = () => {
      element.style.height = "0px";
      element.style.height = `${element.scrollHeight}px`;
    };
    resize();
    return observeResize(resize, element.parentElement!);
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
          fallback={
            <Skeleton className="form-skeleton" label="Loading decision…" />
          }
        />
      ) : !session.generation ? (
        <button
          class="primary"
          disabled={!online || session.presence === "terminal"}
          onClick={() => command("activate", session).catch(report)}
        >
          {session.presence === "terminal"
            ? "Active in terminal"
            : "Resume in this host directory"}
        </button>
      ) : (
        <form onSubmit={submit}>
          <label class="sr-only" for="prompt">
            Message or guidance
          </label>
          <textarea
            id="prompt"
            ref={input}
            rows={1}
            placeholder={running ? "Add guidance…" : "Ask µAgent…"}
            value={draft.text}
            onInput={(event) =>
              setDraft({ ...draft, text: event.currentTarget.value })
            }
            onPaste={(event) => {
              const files = [...(event.clipboardData?.files || [])];
              if (files.length) {
                event.preventDefault();
                upload(files);
              }
            }}
            onKeyDown={(event) => {
              if (
                event.key === "Enter" &&
                !event.shiftKey &&
                !event.isComposing &&
                event.keyCode !== 229
              ) {
                event.preventDefault();
                if (!event.repeat) submit(event);
              }
            }}
          />
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
                    {asset.name}
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
        session={session}
        online={online}
        report={report}
        items={state?.activities || []}
        phase={state?.activity || (state ? "Ready" : "Loading…")}
        running={running}
        pending={pending}
      >
        <div class="metrics">
          <button
            class="quiet context-button"
            aria-label="Raw context"
            title={`${state?.context_tokens?.toLocaleString() || "—"}${state?.context_window ? ` / ${state.context_window.toLocaleString()}` : ""} tokens · View raw context`}
            disabled={!online}
            onClick={showContext}
          >
            {contextSummary(state?.context_tokens, state?.context_window)}
          </button>
          {state?.usage?.cost_reported && (
            <span> · ${state.usage.cost.toFixed(4)}</span>
          )}
        </div>
      </Activities>
    </section>
  );
}
