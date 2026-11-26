import { nameOf } from "../../shared/file-types.ts";
import "./settings.css";
import { ChevronLeft } from "lucide-preact";
import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { useDismiss } from "../../shared/dismiss.ts";
import { usePhone } from "../../shared/layout.ts";
import { usePushDirection } from "../../shared/motion.ts";
import { Button, DialogHeader, Group, Input, Row } from "../../shared/ui.tsx";
import { writeSettingsAddress } from "../../shared/navigation.ts";
import {
  findSettings,
  settingsEntries,
  type SettingsEntry,
} from "../../shared/settings-index.ts";
import {
  parseAddress,
  TOPICS,
  topicOf,
  type Panel,
  type Topic,
} from "../../shared/settings-topics.ts";
import { SettingsContext, type SettingsProps } from "./context.ts";
import {
  ConfigNotices,
  SettingsFile,
  SettingsGroup,
  ShowAll,
} from "./configuration.tsx";
import type { SavedScope } from "./config/facts.ts";
import { useConfiguration } from "./config/use-configuration.ts";
import { SettingRowsLoading } from "./loading.tsx";
import { McpServers } from "./mcp.tsx";
import { AboutPane } from "./panes/about.tsx";
import { NotificationsPane } from "./panes/notifications.tsx";
import { DevicesPane, InstallPane } from "./panes/devices.tsx";
import { DisplayPane } from "./panes/display.tsx";
import { PermissionsPane } from "./panes/permissions.tsx";
import { SettingsNav } from "./settings-nav.tsx";

export default function Settings(props: SettingsProps) {
  const { session, online, catalogue, mcp: servers } = props;
  const phone = usePhone();
  const folder = session?.cwd;
  const [chosenScope, setScope] = useState<SavedScope>("user");
  const scope = folder ? chosenScope : "user";
  const config = useConfiguration(scope, folder, props.version);
  // Null until a topic is picked: a phone shows the list first, and a
  // picked topic is a layer the back gesture returns from.
  const [place, setPlace] = useState(() => parseAddress(props.address));
  const [all, setAll] = useState(false);
  const [query, setQuery] = useState("");
  const [editingInstructions, setEditingInstructions] = useState(false);
  const moved = usePushDirection(place.topic !== undefined);
  useDismiss(phone && place.topic !== undefined, () => go());
  const body = useRef<HTMLDivElement>(null);
  // This conversation is there while one is open.
  const topics = TOPICS.map(({ id }) => id).filter(
    (id) => id !== "conversation" || session,
  );
  const current: Topic = place.topic ?? topics[0];
  const drilled = phone && place.topic !== undefined;
  const topic = topicOf(current)!;
  const go = (address?: string) => {
    setPlace(parseAddress(address));
    setAll(false);
    setQuery("");
  };
  // biome-ignore lint/correctness/useExhaustiveDependencies: follows the address it is opened at
  useEffect(() => go(props.address), [props.address]);
  // A row asked for by its address is shown, folded or not, once drawn.
  const target =
    place.row && config.settings.find((setting) => setting.key === place.row);
  useEffect(() => {
    if (target && target.advanced) setAll(true);
  }, [target]);
  useEffect(() => {
    const pane = body.current;
    if (!pane) return;
    const row = place.row && pane.querySelector(`[data-row="${place.row}"]`);
    if (row) row.scrollIntoView({ block: "start" });
    else pane.scrollTo(0, 0);
  }, [current, place.row, config.loaded, all]);
  // biome-ignore lint/correctness/useExhaustiveDependencies: written when the place changes
  useEffect(
    () =>
      writeSettingsAddress(
        phone && !drilled
          ? ""
          : place.row
            ? `${current}/${place.row}`
            : current,
      ),
    [current, place.row, drilled],
  );
  const results = findSettings(
    settingsEntries(config.settings, config.groups, topics),
    query,
  );
  const found = query.trim() !== "" && (
    <SearchResults results={results} open={(entry) => go(entry.address)} />
  );

  const groups = [
    ...topic.parts.flatMap((part) => ("group" in part ? [part.group] : [])),
    // A group the table does not name still shows, at the end.
    ...config.groups
      .filter(
        ({ id, topic: owner }) =>
          owner === current &&
          !topic.parts.some((part) => "group" in part && part.group === id),
      )
      .map(({ id }) => id),
  ];
  const listed = config.settings.filter((setting) =>
    groups.includes(setting.group),
  );
  const folded = listed.filter((setting) => setting.advanced).length;
  const panels: Partial<Record<Panel, () => ComponentChildren>> = {
    rules: () => <PermissionsPane />,
    "mcp-servers": () => (
      <McpServers
        scope={scope === "project" ? "project" : "global"}
        session={session}
        servers={servers}
        online={online}
      />
    ),
    skills: () => (
      <Group>
        <Row
          label="Skills in the Library"
          detail="Turn skills on or off, or import them."
          onClick={() => props.library("skill")}
        />
      </Group>
    ),
    instructions: () => (
      <Group>
        <Row
          label="Instructions"
          detail="Read by conversations as they start."
          onClick={(event) => {
            event.currentTarget.focus({ preventScroll: true });
            setEditingInstructions(true);
          }}
        />
      </Group>
    ),
    memories: () => (
      <Group>
        <Row
          label="Memories in the Library"
          detail="What the agent remembers."
          onClick={() => props.library("memory")}
        />
      </Group>
    ),
    display: () => <DisplayPane />,
    notifications: () => <NotificationsPane />,
    install: () => <InstallPane />,
    style: () => (
      <Group title="Custom style sheet">
        <Row
          label="style.css"
          detail="~/.uagent/config/style.css on this host restyles every paired device. Edit it by hand, or ask the agent."
        />
      </Group>
    ),
    devices: () => <DevicesPane />,
    about: () => <AboutPane />,
    file: () => <SettingsFile config={config} scope={scope} online={online} />,
  };
  // Panels and groups in the table's order; groups it does not name last.
  const parts = [
    ...topic.parts,
    ...groups
      .slice(topic.parts.filter((part) => "group" in part).length)
      .map((group) => ({ group })),
  ];
  const label = (group: string) =>
    config.groups.find(({ id }) => id === group)?.label ?? group;
  return (
    <SettingsContext.Provider value={props}>
      <DialogHeader
        title={drilled ? topic.label : "Settings"}
        leading={
          drilled && (
            <Button variant="quiet" onClick={() => go()}>
              <ChevronLeft />
              Back
            </Button>
          )
        }
      />
      <div
        class="dialog-body settings-content"
        data-drilled={place.topic ? "" : undefined}
        data-direction={moved === "back" ? "back" : undefined}
      >
        <SettingsNav
          topics={topics}
          current={phone ? undefined : current}
          select={(id) => go(id)}
          details={{ conversation: session?.title || "Conversation" }}
          class={moved && "push"}
          search={
            <Input
              type="search"
              class="settings-search"
              aria-label="Find a setting"
              placeholder="Find a setting"
              value={query}
              onInput={(event) => setQuery(event.currentTarget.value)}
            />
          }
        >
          {phone && found}
        </SettingsNav>
        {!phone && found ? (
          <div class="settings-pane">{found}</div>
        ) : (
          (!phone || drilled) && (
            <div
              ref={body}
              class={moved ? "settings-pane push" : "settings-pane"}
            >
              {!phone && <h3 class="settings-pane-title">{topic.label}</h3>}
              {groups.length > 0 && folder && (
                <div
                  class="segmented settings-scope"
                  role="group"
                  aria-label="Where it applies"
                >
                  <Button
                    aria-pressed={scope === "user"}
                    onClick={() => setScope("user")}
                  >
                    All conversations
                  </Button>
                  <Button
                    aria-pressed={scope === "project"}
                    onClick={() => setScope("project")}
                  >
                    <span class="truncate">This folder · {nameOf(folder)}</span>
                  </Button>
                </div>
              )}
              {groups.length > 0 && (
                <ConfigNotices
                  config={config}
                  scope={scope}
                  folder={folder}
                  sessions={catalogue.sessions}
                />
              )}
              {groups.length > 0 && !config.loaded && config.error == null && (
                <SettingRowsLoading />
              )}
              {parts.map((part) => {
                if (!("group" in part))
                  return (
                    <div key={part.panel} data-row={part.panel}>
                      {(
                        panels[part.panel] ??
                        (() => props.panels[part.panel]?.(go, place.row))
                      )()}
                    </div>
                  );
                const own = listed.filter(
                  (setting) =>
                    setting.group === part.group && (all || !setting.advanced),
                );
                return (
                  config.loaded && (
                    <SettingsGroup
                      key={part.group}
                      config={config}
                      label={label(part.group)}
                      settings={own}
                      scope={scope}
                      folder={folder}
                      online={online}
                    />
                  )
                );
              })}
              {config.loaded && folded > 0 && (
                <ShowAll
                  count={folded}
                  shown={all}
                  toggle={() => setAll(!all)}
                />
              )}
            </div>
          )
        )}
      </div>
      {editingInstructions &&
        props.instructions(() => setEditingInstructions(false))}
    </SettingsContext.Provider>
  );
}

function SearchResults({
  results,
  open,
}: {
  results: SettingsEntry[];
  open: (entry: SettingsEntry) => void;
}) {
  return (
    <Group title="Found">
      {!results.length && <Row label="Nothing matches" />}
      {results.map((entry) => (
        <Row
          key={entry.address}
          label={entry.label}
          detail={[entry.detail, entry.topic].filter(Boolean).join(" · ")}
          onClick={() => open(entry)}
        />
      ))}
    </Group>
  );
}
