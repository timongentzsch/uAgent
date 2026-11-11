import { useEffect, useState } from "preact/hooks";
import Markdown from "../../../shared/markdown-view.tsx";
import { applyUpdate } from "../../../shared/pwa.ts";
import type { UpdateState } from "../../../shared/types.ts";
import {
  Actions,
  Button,
  CodeCopy,
  Group,
  LoadError,
  Modal,
  Row,
  Time,
} from "../../../shared/ui.tsx";
import { useAction } from "../../../shared/use-action.ts";
import { useApi } from "../../../state/api.ts";
import { useSettings } from "../context.ts";

// How the installer says µAgent was installed, and where its update runs
// when not from here.
const INSTALLED: Record<string, [string, string]> = {
  release: ["A release", "Run this on the host."],
  source: ["A release, built on this host", "Run this on the host."],
  checkout: ["A checkout", "Run this in the checkout it was built from."],
  container: [
    "The container image",
    "Run this on the machine that runs Docker; what the container keeps stays on its volume.",
  ],
  "": ["Not recorded", "Run this on the host."],
};
const BLOCKED: Record<string, string> = {
  terminal: "The terminal is turned off for paired devices, and so is this.",
  unwritable: "The folder µAgent is installed in cannot be written from here.",
};
const POLL_MS = 2000;

export function AboutPane() {
  const { manage } = useApi();
  const { update, updateBlocked } = useSettings();
  const action = useAction();
  const [state, setState] = useState<UpdateState>();
  const [open, setOpen] = useState<"notes" | "confirm">();
  const ask = (act: "show" | "check" | "apply", version?: string) =>
    manage("update", { action: act, version }).then(setState);
  // biome-ignore lint/correctness/useExhaustiveDependencies: read once
  useEffect(() => {
    void ask("show").catch(() => {});
  }, []);
  // The host restarts when its installer is done: asked until it answers
  // again.
  const installing = state?.installing;
  // biome-ignore lint/correctness/useExhaustiveDependencies: `ask` is new each render
  useEffect(() => {
    if (!installing) return;
    const timer = setInterval(() => void ask("show").catch(() => {}), POLL_MS);
    return () => clearInterval(timer);
  }, [installing]);
  const [installed, where] = INSTALLED[state?.install ?? ""] ?? INSTALLED[""];
  return (
    <>
      <Group title="µAgent">
        <Row label="Version" detail={state?.version} />
        <Row label="Installed as" detail={state && installed} />
      </Group>
      <Group
        title="Updates"
        footer={
          state?.checked_ms ? (
            <>
              Checked <Time value={state.checked_ms} />. Releases are read from
              GitHub.
            </>
          ) : (
            "Releases are read from GitHub when you ask."
          )
        }
      >
        <Row
          label={
            state?.available
              ? `µAgent ${state.latest} is available`
              : state?.latest
                ? "Up to date"
                : "Not checked yet"
          }
        >
          <Button
            size="compact"
            busy={action.busy}
            disabled={installing}
            onClick={() => void action.run(() => ask("check"))}
          >
            Check for updates
          </Button>
        </Row>
        {state?.available && state.notes && (
          <Row label="What is new" onClick={() => setOpen("notes")} />
        )}
        {installing ? (
          <Row
            label={`Installing µAgent ${state.latest}…`}
            detail="The host restarts when it is done; this page reconnects by itself."
          />
        ) : state?.apply ? (
          <Row
            label={`Update to ${state.latest}`}
            detail="Conversations keep running and restart once idle."
            onClick={() => setOpen("confirm")}
          />
        ) : (
          state?.available && (
            <Row
              label={<code>{state.command}</code>}
              detail={`${BLOCKED[state.blocked] ?? ""} ${where}`.trim()}
            >
              <CodeCopy text={state.command} />
            </Row>
          )
        )}
        {state?.error && (
          <Row
            label={
              state.output === undefined
                ? "GitHub could not be asked"
                : "The update was not installed"
            }
            detail={state.output?.trim().split("\n").at(-1) || state.error}
          />
        )}
      </Group>
      {action.error != null && <LoadError error={action.error} />}
      {update && (
        <Group
          title="This page"
          footer="Updating reloads this view. Send or copy unsent drafts first."
        >
          <Row
            label="Apply update"
            disabled={updateBlocked}
            onClick={() => applyUpdate(update)}
          />
        </Group>
      )}
      {open === "notes" && state && (
        <Modal
          title={`µAgent ${state.latest}`}
          close={() => setOpen(undefined)}
        >
          <Markdown text={state.notes ?? ""} />
          {state.url && (
            <Actions>
              <Button
                onClick={() => window.open(state.url, "_blank", "noopener")}
              >
                Open on GitHub
              </Button>
            </Actions>
          )}
        </Modal>
      )}
      {open === "confirm" && state && (
        <Modal
          title={`Update to ${state.latest}`}
          close={() => setOpen(undefined)}
        >
          <p>
            The host downloads µAgent {state.latest} from GitHub, replaces this
            install and restarts. Conversations keep running and restart once
            idle.
          </p>
          <Actions>
            <Button onClick={() => setOpen(undefined)}>Cancel</Button>
            <Button
              variant="primary"
              onClick={() => {
                setOpen(undefined);
                void action.run(() => ask("apply", state.latest));
              }}
            >
              Update
            </Button>
          </Actions>
        </Modal>
      )}
    </>
  );
}
