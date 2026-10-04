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
import {
  SCOPES,
  SECTIONS,
  SettingsNav,
  type Section,
} from "./settings-nav.tsx";
const configuration = () => import("./configuration.tsx");

// Sections embed the registry settings they own; Advanced and a project
// list them all.
const CONFIG: Partial<
  Record<Section, ((setting: ConfigSetting) => boolean) | undefined>
> = {
  general: (setting) =>
    setting.name === "UAGENT_VERBOSITY" ||
    (["memory", "skills", "delegation"].includes(setting.category) &&
      !setting.name.endsWith("_MODEL")),
  models: (setting) =>
    setting.category === "route" || setting.name.endsWith("_MODEL"),
  permissions: (setting) =>
    setting.name === "UAGENT_APPROVAL" ||
    setting.name.startsWith("UAGENT_PERMISSION_"),
  advanced: undefined,
  project: undefined,
};
// The models first, each a role; then how they are reached.
const MODEL_SECTIONS: [string, (setting: ConfigSetting) => boolean][] = [
  ["Models", (setting) => setting.name.endsWith("_MODEL")],
  ["Connection", () => true],
];
// A conversation's own choices are made in the conversation: the sections
// that hold their defaults say where.
const CHOSEN: Partial<Record<Section, string>> = {
  models:
    "New conversations start on the conversation model. A conversation's own model is chosen in its composer.",
  permissions:
    "The approval mode is what a conversation uses until its own is chosen in its composer.",
};

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
  // A project's section needs its conversation.
  const current =
    (section === "project" && !session ? null : section) || "general";
  const [, title, , scope] = SECTIONS.find(([id]) => id === current)!;
  const folder = session?.cwd?.split("/").pop();
  const drilled = phone && section !== null;
  const state = snapshots[selected]?.state;
  const instructions = (label: string, detail: string) => (
    <Group>
      <Row label={label} detail={detail} onClick={props.instructions} />
    </Group>
  );
  const panes: Partial<Record<Section, preact.ComponentChildren>> = {
    general: instructions(
      "Instructions",
      "What every session and each folder's coordinator read at start.",
    ),
    mcp: (
      <McpServers
        scope="global"
        session={session}
        state={state}
        online={online}
      />
    ),
    project: (
      <>
        <McpServers
          scope="project"
          session={session}
          state={state}
          online={online}
        />
        <PermissionsPane />
        {instructions(
          "Project instructions",
          "What sessions and the coordinator read at start in this folder.",
        )}
      </>
    ),
    display: <DisplayPane />,
    devices: <DevicesPane />,
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
          project={session && (folder || "project")}
        />
        <div ref={body} class="settings-pane">
          {!phone && <h3 class="settings-pane-title">{title}</h3>}
          <p class="settings-scope">
            {!phone && `${SCOPES[scope][0]}. `}
            {SCOPES[scope][1](session?.cwd)}
          </p>
          {/* One instance in one slot: switching sections refilters the
              catalogue it already loaded instead of fetching it again. */}
          {current in CONFIG && (
            <Deferred
              load={configuration}
              key={scope}
              session={session}
              online={online}
              scope={scope === "project" ? "project" : "user"}
              sessions={catalogue.sessions}
              display={
                current === "advanced"
                  ? {
                      changed: displayChanged(props),
                      reset: () => resetDisplay(props),
                    }
                  : undefined
              }
              filter={CONFIG[current]}
              sections={current === "models" ? MODEL_SECTIONS : undefined}
              fallback={<SettingRowsLoading />}
            />
          )}
          {CHOSEN[current] && <p class="group-footer">{CHOSEN[current]}</p>}
          {panes[current]}
        </div>
      </div>
    </SettingsContext.Provider>
  );
}
