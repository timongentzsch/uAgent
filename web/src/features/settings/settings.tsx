import "./settings.css";
import { ChevronLeft } from "lucide-preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { useDismiss } from "../../shared/dismiss.ts";
import { usePhone } from "../../shared/layout.ts";
import {
  Button,
  Deferred,
  DialogHeader,
  Group,
  Row,
} from "../../shared/ui.tsx";
import { SettingsContext, type SettingsProps } from "./context.ts";
import { SettingRowsLoading } from "./loading.tsx";
import { McpServers } from "./mcp.tsx";
import { DevicesPane } from "./panes/devices.tsx";
import { DisplayPane } from "./panes/display.tsx";
import { PermissionsPane } from "./panes/permissions.tsx";
import { SCOPES, SettingsNav, type Scope } from "./settings-nav.tsx";
const configuration = () => import("./configuration.tsx");

// The shell: which scope is open, the list of scopes, and its page.
export default function Settings(props: SettingsProps) {
  const { session, selected, online, catalogue, snapshots } = props;
  // Null until a scope is picked: a phone shows the list first, and a
  // picked scope is a layer the back gesture returns from.
  const [picked, setPicked] = useState<Scope | null>(
    () => SCOPES.find(([id]) => id === props.initialSection)?.[0] ?? null,
  );
  const phone = usePhone();
  useDismiss(phone && picked !== null, () => setPicked(null));
  const body = useRef<HTMLDivElement>(null);
  useEffect(() => {
    body.current?.scrollTo(0, 0);
  }, [picked]);
  // A project's page needs its conversation.
  const current = (picked === "project" && !session ? null : picked) || "user";
  const [, title, , describe] = SCOPES.find(([id]) => id === current)!;
  const folder = session?.cwd?.split("/").pop();
  const drilled = phone && picked !== null;
  const state = snapshots[selected]?.state;
  const instructions = (label: string, detail: string) => (
    <Group>
      <Row label={label} detail={detail} onClick={props.instructions} />
    </Group>
  );
  const mcp = (scope: "global" | "project") => (
    <McpServers scope={scope} session={session} state={state} online={online} />
  );
  const pages: Record<Scope, preact.ComponentChildren> = {
    user: (
      <>
        {instructions(
          "Instructions",
          "What every conversation and each folder's coordinator read at start.",
        )}
        {mcp("global")}
      </>
    ),
    project: (
      <>
        {instructions(
          "Project instructions",
          "What conversations and the coordinator read at start in this folder.",
        )}
        {mcp("project")}
        <PermissionsPane />
      </>
    ),
    browser: <DisplayPane />,
    host: <DevicesPane />,
  };
  return (
    <SettingsContext.Provider value={props}>
      <DialogHeader
        title={drilled ? title : "Settings"}
        leading={
          drilled && (
            <Button variant="quiet" onClick={() => setPicked(null)}>
              <ChevronLeft />
              Back
            </Button>
          )
        }
      />
      <div
        class="dialog-body settings-content"
        data-drilled={picked ? "" : undefined}
      >
        <SettingsNav
          current={phone ? undefined : current}
          select={setPicked}
          project={session && (folder || "project")}
        />
        <div ref={body} class="settings-pane">
          {!phone && <h3 class="settings-pane-title">{title}</h3>}
          <p class="settings-scope">{describe(session?.cwd)}</p>
          {pages[current]}
          {(current === "user" || current === "project") && (
            <Deferred
              load={configuration}
              key={current + session?.cwd}
              scope={current}
              folder={session?.cwd}
              version={props.version}
              online={online}
              sessions={catalogue.sessions}
              fallback={<SettingRowsLoading />}
            />
          )}
        </div>
      </div>
    </SettingsContext.Provider>
  );
}
