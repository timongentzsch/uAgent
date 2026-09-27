import {
  ArrowUp,
  ChevronDown,
  ChevronUp,
  Gauge,
  Layers,
  Paperclip,
  Shield,
} from "lucide-preact";
import {
  Button,
  DialogHeader,
  Field,
  Group,
  IconButton,
  SettingRow,
  Skeleton,
} from "./ui.tsx";
import { Textarea } from "./form-controls.tsx";
import { Popover } from "./popover.tsx";
import { ConnectionStatus } from "./connection-status.tsx";
import type { ComponentChildren } from "preact";
import "../features/settings/settings.css";
import { SettingsNav } from "../features/settings/settings-nav.tsx";
import { useMedia } from "./layout.ts";

const noop = () => {};

const busy = { role: "status", "aria-busy": true } as const;

// Placeholders only for known fields and text rows. Feature shells own their
// real controls; unknown content uses Spinner instead of copying a screen.
function Control() {
  return <Skeleton decorative rows={1} className="control-skeleton" />;
}
// The transcript before its first blocks. The app shell, the chat chunk and
// the snapshot each render this same placeholder, immediately, so loading
// reads as one calm state instead of a chain of spinners.
export function TranscriptLoading() {
  return (
    <>
      <span className="sr-only" role="status" aria-busy="true">
        Loading conversation…
      </span>
      <Skeleton decorative rows={5} className="transcript-skeleton" />
    </>
  );
}

// The composer before its code or the session arrives: the live elements,
// inert, with placeholder text where data goes, so nothing below the
// transcript moves when it hydrates.
export function ComposerLoading() {
  const trigger = (icon: ComponentChildren, text: string) => (
    <>
      {icon}
      <span class="text-skeleton">{text}</span>
    </>
  );
  return (
    <section class="composer" aria-hidden="true">
      <div class="activities">
        <div class="status-line">
          <span class="activity-toggle">
            <ConnectionStatus phase="connecting" />
          </span>
        </div>
      </div>
      <form>
        <Textarea
          class="message-input"
          rows={1}
          placeholder="Ask µAgent…"
          tabIndex={-1}
          disabled
        />
        <div class="composer-actions">
          <span class="file-button icon-button">
            <Paperclip />
          </span>
          <Popover
            label="Model and effort"
            className="model-control"
            buttonClass="quiet model-selector with-icon"
            disabled
            trigger={
              <>
                {trigger(<Gauge />, "provider/model")}
                <ChevronDown />
              </>
            }
          >
            {() => null}
          </Popover>
          <Popover
            label="Permissions"
            className="permission-control"
            buttonClass="quiet with-icon"
            disabled
            trigger={trigger(<Shield />, "Ask")}
          >
            {() => null}
          </Popover>
          <IconButton label="Send" variant="primary" disabled>
            <ArrowUp />
          </IconButton>
        </div>
      </form>
      <div class="composer-metrics">
        <Button variant="quiet" class="activity-button idle" disabled>
          <Layers aria-hidden="true" />
          <span class="text-skeleton">Background</span>
          <ChevronUp aria-hidden="true" />
        </Button>
        <div class="metrics">
          <Button variant="quiet" disabled>
            <span class="text-skeleton">est. ctx 0k/0M</span>
          </Button>
          <Button variant="quiet" disabled>
            <span class="text-skeleton">Session · 0 turns</span>
          </Button>
        </div>
      </div>
    </section>
  );
}

// Settings before its code arrives: the same header, section list and
// pane title.
export function SettingsLoading() {
  const phone = useMedia("(max-width: 600px)");
  return (
    <>
      <DialogHeader title="Settings" />
      <div class="dialog-body settings-content">
        <span
          className="sr-only"
          role="status"
          aria-busy="true"
          aria-label="Loading settings…"
        >
          Loading settings…
        </span>
        <SettingsNav current={phone ? undefined : "general"} select={noop} />
        <div class="settings-pane">
          {!phone && <h3 class="settings-pane-title">General</h3>}
        </div>
      </div>
    </>
  );
}

// Registry settings before they load: rows in the live row, with
// placeholder text for the name, description and value.
export function SettingRowsLoading() {
  return (
    <>
      <span class="sr-only" role="status" aria-busy="true">
        Loading configuration…
      </span>
      <div aria-hidden="true">
        <Group>
          {["UAGENT_MODEL", "UAGENT_API_KEY", "UAGENT_BASE_URL"].map((name) => (
            <SettingRow
              key={name}
              name={name}
              label={<span class="text-skeleton">{name}</span>}
              detail={
                <span class="text-skeleton">
                  What this setting changes and where it is set
                </span>
              }
              reset={noop}
            >
              <span class="text-skeleton">Not set</span>
            </SettingRow>
          ))}
        </Group>
      </div>
    </>
  );
}

function ModelSkeleton() {
  // Both callers own the form wrapper and actions. Reserve the optional
  // variant slot until the catalogue tells us whether it is available.
  return (
    <>
      <span className="sr-only" role="status" aria-busy="true">
        Loading models…
      </span>
      <Field label="Model · loading…">
        <Control />
      </Field>
      <div className="field-row" aria-hidden="true">
        <Field label="Effort">
          <Control />
        </Field>
        <Field label="Variant">
          <Control />
        </Field>
      </div>
    </>
  );
}

export function ModelLoading({ close }: { close: () => void }) {
  return (
    <div class="model-form">
      <ModelSkeleton />
      <ModelActions close={close} />
    </div>
  );
}

export function ModelActions({
  close,
  apply,
  disabled = false,
  busy = false,
}: {
  close: () => void;
  apply?: () => void;
  disabled?: boolean;
  busy?: boolean;
}) {
  return (
    <div class="dialog-actions">
      <button type="button" onClick={close}>
        Cancel
      </button>
      <button
        type="button"
        class="primary"
        disabled={!apply || disabled || busy}
        onClick={apply}
      >
        {busy ? "Applying…" : "Apply"}
      </button>
    </div>
  );
}

export function LibraryRows({ count = 3 }: { count?: number }) {
  return (
    <div {...busy} aria-label="Loading library…">
      <ManagementRows count={count} />
    </div>
  );
}

function ManagementRows({ count = 3 }: { count?: number }) {
  return Array.from({ length: count }, (_, i) => (
    <div key={i} className="library-row" aria-hidden="true">
      <Skeleton decorative rows={1} className="title-skeleton" />
      <Skeleton decorative rows={1} className="meta-skeleton" />
    </div>
  ));
}
