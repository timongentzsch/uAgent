import "./settings.css";
import { ChevronLeft } from "lucide-preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { ConfigSetting } from "../../shared/types.ts";
import { useDismiss } from "../../shared/dismiss.ts";
import { useMedia } from "../../shared/layout.ts";
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
import { DisplayPane, displayChanged, resetDisplay } from "./panes/display.tsx";
import { PermissionsPane } from "./panes/permissions.tsx";
import { SECTIONS, SettingsNav, type Section } from "./settings-nav.tsx";
const configuration = () => import("./configuration.tsx");

// Sections embed the registry settings they own; Advanced lists them all.
const CONFIG: Partial<
  Record<Section, ((setting: ConfigSetting) => boolean) | undefined>
> = {
  models: (setting) =>
    setting.category === "route" || setting.name.endsWith("_MODEL"),
  permissions: (setting) =>
    setting.name === "UAGENT_APPROVAL" ||
    setting.name.startsWith("UAGENT_PERMISSION_"),
  agent: (setting) =>
    ["memory", "skills", "delegation"].includes(setting.category) &&
    !setting.name.endsWith("_MODEL"),
  advanced: undefined,
};
// The models first, each a role; then how they are reached.
const MODEL_SECTIONS: [string, (setting: ConfigSetting) => boolean][] = [
  ["Models", (setting) => setting.name.endsWith("_MODEL")],
  ["Connection", () => true],
];

// The shell: which section is open, the list of sections, and its pane.
export default function Settings(props: SettingsProps) {
  const { session, selected, online, catalogue, snapshots } = props;
  // Null until a section is picked: a phone shows the section list first,
  // and a picked section is a layer the back gesture returns from.
  const [section, setSection] = useState<Section | null>(
    () => SECTIONS.find(([id]) => id === props.initialSection)?.[0] ?? null,
  );
  const phone = useMedia("(max-width: 600px)");
  useDismiss(phone && section !== null, () => setSection(null));
  const body = useRef<HTMLDivElement>(null);
  useEffect(() => {
    body.current?.scrollTo(0, 0);
  }, [section]);
  const current = section || "general";
  const title = SECTIONS.find(([id]) => id === current)![1];
  const drilled = phone && section !== null;
  const panes: Partial<Record<Section, preact.ComponentChildren>> = {
    general: <DisplayPane />,
    permissions: <PermissionsPane />,
    agent: (
      <Group>
        <Row
          label="Instructions"
          detail="What every session and each folder's coordinator read at start."
          onClick={props.instructions}
        />
      </Group>
    ),
    tools: (
      <Group>
        <Row
          label="Tools for this conversation"
          detail={
            selected
              ? "Which tools the open conversation may use."
              : "Open a conversation to choose its tools."
          }
          disabled={!selected}
          onClick={props.tools}
        />
      </Group>
    ),
    devices: <DevicesPane />,
    mcp: (
      <McpServers
        session={session}
        state={snapshots[selected]?.state}
        online={online}
      />
    ),
  };
  return (
    <SettingsContext.Provider value={props}>
      <DialogHeader
        title={drilled ? title : "Settings"}
        leading={
          drilled && (
            <Button variant="quiet" onClick={() => setSection(null)}>
              <ChevronLeft />
              Back
            </Button>
          )
        }
      />
      <div
        class="dialog-body settings-content"
        data-drilled={section ? "" : undefined}
      >
        <SettingsNav
          current={phone ? undefined : current}
          select={setSection}
        />
        <div ref={body} class="settings-pane">
          {!phone && <h3 class="settings-pane-title">{title}</h3>}
          {/* One instance in one slot: switching sections refilters the
              catalogue it already loaded instead of fetching it again. */}
          {current in CONFIG && (
            <Deferred
              load={configuration}
              session={session}
              online={online}
              sessions={catalogue.sessions}
              display={{
                changed: displayChanged(props),
                reset: () => resetDisplay(props),
              }}
              filter={CONFIG[current]}
              sections={current === "models" ? MODEL_SECTIONS : undefined}
              fallback={<SettingRowsLoading />}
            />
          )}
          {panes[current]}
        </div>
      </div>
    </SettingsContext.Provider>
  );
}
