import "./settings.css";
import { ZoomSlider } from "../../shared/zoom-slider.tsx";

import type { Dispatch, StateUpdater, MutableRef } from "preact/hooks";
import type {
  InstallPrompt,
  Snapshot,
  Catalogue,
  PermissionRules,
  Session,
  CommandFields,
} from "../../shared/types.ts";
import {
  Button,
  Deferred,
  DialogHeader,
  Group,
  LoadError,
  Row,
  SettingRow,
  Switch,
  ValueSelect,
} from "../../shared/ui.tsx";
import { useEffect, useRef, useState } from "preact/hooks";
import { SettingRowsLoading } from "./loading.tsx";
const configuration = () => import("./configuration.tsx");
import { api, command } from "../../state/api.ts";
import { ChevronLeft } from "lucide-preact";
import { SECTIONS, SettingsNav, type Section } from "./settings-nav.tsx";
import { useMedia } from "../../shared/layout.ts";
import { McpServers } from "./mcp.tsx";
import { applyUpdate } from "../../shared/pwa.ts";
import { useDismiss } from "../../shared/dismiss.ts";
import { defaultTimePrefs, type TimePrefs } from "../../shared/time.ts";
import type { ConfigSetting } from "../../shared/types.ts";

// This device's display settings and their defaults: stored in the browser,
// shown and reset like every other setting.
const DISPLAY = { theme: "system", zoom: 100 } as const;
const TIMESTAMP_STYLES: Record<TimePrefs["style"], string> = {
  smart: "Time today, the date when older.",
  relative: "How long ago, e.g. 5 min ago.",
  absolute: "The full date and time.",
};
// Sections embed the registry settings they own; Advanced lists them all.
const models = (setting: ConfigSetting) =>
  setting.category === "route" || setting.name.endsWith("_MODEL");
const permissions = (setting: ConfigSetting) =>
  setting.name === "UAGENT_APPROVAL" ||
  setting.name.startsWith("UAGENT_PERMISSION_");
const agent = (setting: ConfigSetting) =>
  ["memory", "skills", "delegation"].includes(setting.category) &&
  !setting.name.endsWith("_MODEL");
const CONFIG = { models, permissions, agent, advanced: undefined };
export default function Settings({
  theme,
  setTheme,
  timePrefs,
  setTimePrefs,
  zoom,
  setZoom,
  installed,
  install,
  setInstall,
  ios,
  update,
  updateBlocked,
  snapshots,
  catalogue,
  online,
  notificationMode,
  setNotificationMode,
  notifications,
  refresh,
  selected,
  session,
  logout,
  instructions,
  tools,
  usage,
  initialSection,
}: {
  theme: string;
  setTheme: Dispatch<StateUpdater<string>>;
  timePrefs: TimePrefs;
  setTimePrefs: Dispatch<StateUpdater<TimePrefs>>;
  zoom: number;
  setZoom: Dispatch<StateUpdater<number>>;
  installed: boolean;
  install: InstallPrompt | null;
  setInstall: Dispatch<StateUpdater<InstallPrompt | null>>;
  ios: boolean;
  update: ServiceWorker | null;
  // Unsent drafts or a waiting decision hold the reload.
  updateBlocked: boolean;
  snapshots: Record<string, Snapshot>;
  catalogue: Catalogue;
  online: boolean;
  notificationMode: boolean;
  setNotificationMode: Dispatch<StateUpdater<boolean>>;
  notifications: MutableRef<boolean>;
  refresh: () => Promise<void>;
  selected: string;
  session?: Session;
  logout: () => Promise<void>;
  instructions: () => void;
  // The open conversation's tools and usage, in their own sheets.
  tools: () => void;
  usage: () => void;
  initialSection?: string;
}) {
  // Null until a section is picked: a phone shows the section list first,
  // and a picked section is a layer the back gesture returns from.
  const [section, setSection] = useState<Section | null>(
    () => SECTIONS.find(([id]) => id === initialSection)?.[0] ?? null,
  );
  const phone = useMedia("(max-width: 600px)");
  useDismiss(phone && section !== null, () => setSection(null));
  const [error, setError] = useState<unknown>(null);
  const report = setError;
  const body = useRef<HTMLDivElement>(null);
  useEffect(() => {
    body.current?.scrollTo(0, 0);
  }, [section]);
  const [rules, setRules] = useState<PermissionRules | null>(null);
  const [ruleError, setRuleError] = useState<unknown>(null);
  useEffect(() => {
    if (!online || !session?.cwd) {
      setRules(null);
      return;
    }
    let active = true;
    command("permission_rules", null, {
      action: "list",
      cwd: session.cwd,
    })
      .then((response) => {
        if (active && !response.pending) setRules(response.result);
      })
      .catch((failure) => {
        if (active) setRuleError(failure);
      });
    return () => {
      active = false;
    };
  }, [online, session?.cwd]);
  // Forgetting one rule or all of them answers with the rules left.
  const editRules = (fields: CommandFields) => {
    setRuleError(null);
    command("permission_rules", null, { ...fields, cwd: session?.cwd })
      .then((response) => {
        if (!response.pending) setRules(response.result);
      })
      .catch(setRuleError);
  };
  // Turning notifications on subscribes this device to push where the host
  // offers it, otherwise to notifications while this view is connected.
  const notificationsOn =
    !!catalogue.capabilities.subscribed || notificationMode;
  async function setNotifications(on: boolean) {
    try {
      if (!on) {
        await api("/api/push/subscriptions", undefined, { method: "DELETE" });
        const registration = await navigator.serviceWorker.getRegistration();
        await (
          await registration?.pushManager?.getSubscription()
        )?.unsubscribe();
        setNotificationMode(false);
        notifications.current = false;
      } else {
        if ((await Notification.requestPermission()) !== "granted")
          throw new Error("Notification permission was not granted.");
        if (catalogue.capabilities.push && "PushManager" in globalThis) {
          const registration = await navigator.serviceWorker.ready;
          const key = (catalogue.capabilities.vapid_public_key || "")
            .replace(/-/g, "+")
            .replace(/_/g, "/");
          const subscription =
            (await registration.pushManager.getSubscription()) ||
            (await registration.pushManager.subscribe({
              userVisibleOnly: true,
              applicationServerKey: Uint8Array.from(atob(key), (value) =>
                value.charCodeAt(0),
              ),
            }));
          await api("/api/push/subscriptions", subscription.toJSON());
        } else {
          setNotificationMode(true);
          notifications.current = true;
        }
      }
      await refresh();
    } catch (failure) {
      report(failure);
    }
  }
  const pane = {
    general: (
      <Group title="Display" footer="Saved on this device.">
        <SettingRow
          name="Appearance"
          htmlFor="appearance"
          overridden={theme !== DISPLAY.theme}
          reset={() => setTheme(DISPLAY.theme)}
        >
          <ValueSelect
            id="appearance"
            value={theme}
            onChange={(event) => setTheme(event.currentTarget.value)}
          >
            <option value="system">System</option>
            <option value="dark">Dark</option>
            <option value="light">Light</option>
          </ValueSelect>
        </SettingRow>
        <SettingRow
          name="Clock"
          htmlFor="clock"
          overridden={timePrefs.clock !== defaultTimePrefs.clock}
          reset={() =>
            setTimePrefs({ ...timePrefs, clock: defaultTimePrefs.clock })
          }
        >
          <ValueSelect
            id="clock"
            value={timePrefs.clock}
            onChange={(event) =>
              setTimePrefs({
                ...timePrefs,
                clock: event.currentTarget.value as TimePrefs["clock"],
              })
            }
          >
            <option value="system">System</option>
            <option value="12">12-hour</option>
            <option value="24">24-hour</option>
          </ValueSelect>
        </SettingRow>
        <SettingRow
          name="Timestamps"
          htmlFor="timestamps"
          detail={TIMESTAMP_STYLES[timePrefs.style]}
          overridden={timePrefs.style !== defaultTimePrefs.style}
          reset={() =>
            setTimePrefs({ ...timePrefs, style: defaultTimePrefs.style })
          }
        >
          <ValueSelect
            id="timestamps"
            value={timePrefs.style}
            onChange={(event) =>
              setTimePrefs({
                ...timePrefs,
                style: event.currentTarget.value as TimePrefs["style"],
              })
            }
          >
            <option value="smart">Smart</option>
            <option value="relative">Relative</option>
            <option value="absolute">Absolute</option>
          </ValueSelect>
        </SettingRow>
        <SettingRow
          name="Zoom"
          htmlFor="zoom"
          detail="Scales the entire interface, conversation included — like browser zoom."
          overridden={zoom !== DISPLAY.zoom}
          reset={() => setZoom(DISPLAY.zoom)}
        >
          <ZoomSlider zoom={zoom} change={setZoom} />
        </SettingRow>
      </Group>
    ),
    models: null,
    permissions: session?.cwd && (
      <Group
        title={`Remembered for this repository${
          rules?.rules.length ? ` · ${rules.rules.length}` : ""
        }`}
        footer="Exact actions allowed for this repository. Tool definitions and arguments must still match."
      >
        {ruleError && (
          <div class="group-block">
            <LoadError error={ruleError} />
          </div>
        )}
        {rules?.rules.map((rule) => (
          <Row key={rule.key} label={rule.tool} detail={rule.preview}>
            <Button
              variant="quiet"
              size="compact"
              disabled={!online}
              onClick={() => editRules({ action: "delete", key: rule.key })}
            >
              Forget
            </Button>
          </Row>
        ))}
        {rules && !rules.rules.length && <Row label="No remembered actions." />}
        {!!rules?.rules.length && (
          <Row
            label="Forget all for this repository"
            destructive
            disabled={!online}
            onClick={() => editRules({ action: "clear" })}
          />
        )}
      </Group>
    ),
    agent: (
      <Group>
        <Row
          label="Instructions"
          detail="What every session and each folder's coordinator read at start."
          onClick={instructions}
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
          onClick={tools}
        />
      </Group>
    ),
    usage: (
      <Group>
        <Row
          label="This conversation's usage"
          detail={
            selected
              ? "Tokens, cost and time spent so far."
              : "Open a conversation to see its usage."
          }
          disabled={!selected}
          onClick={usage}
        />
      </Group>
    ),
    devices: (
      <>
        {error && <LoadError error={error} />}
        {update && (
          <Group
            title="Update"
            footer="Updating reloads this view. Send or copy unsent drafts first."
          >
            <Row
              label="Apply update"
              disabled={updateBlocked}
              onClick={() => applyUpdate(update)}
            />
          </Group>
        )}
        <Group title="Install">
          {installed ? (
            <Row label="Installed" detail="Running as an installed app." />
          ) : install ? (
            <Row
              label="Install µAgent"
              onClick={async () => {
                try {
                  await install.prompt();
                } catch (failure) {
                  report(failure);
                }
                setInstall(null);
              }}
            />
          ) : (
            <Row
              label="Install"
              detail={
                ios
                  ? "In Safari, choose Share → Add to Home Screen. Open the installed app and pair it if needed."
                  : "Use your browser’s install option where supported. Installation needs a trusted HTTPS origin or localhost."
              }
            />
          )}
        </Group>
        <Group
          title="Notifications"
          footer={
            <>
              {catalogue.capabilities.subscribed
                ? "Background notifications enabled for this device."
                : notificationMode
                  ? "Connected-view notifications enabled. Delivery requires this browser view to remain connected."
                  : "Notifications are muted for this device."}{" "}
              Focus settings, power management and network conditions can delay
              delivery. On iOS, enable notifications from the installed Home
              Screen app.
            </>
          }
        >
          <Row
            label="Notifications"
            detail={
              catalogue.capabilities.push
                ? "Background push, for supported installed browsers."
                : "While this view is connected."
            }
          >
            <Switch
              label="Notifications"
              checked={notificationsOn}
              disabled={
                !online ||
                !isSecureContext ||
                !("Notification" in globalThis) ||
                !("serviceWorker" in navigator)
              }
              onChange={(on) => void setNotifications(on)}
            />
          </Row>
          {notificationsOn && (
            <Row
              label="Send test notification"
              disabled={!online || !selected}
              onClick={async () => {
                try {
                  if (catalogue.capabilities.subscribed)
                    await command("test_notification", session);
                  else
                    (await navigator.serviceWorker.ready).active?.postMessage({
                      type: "ATTENTION",
                      id: crypto.randomUUID(),
                      session_id: selected,
                    });
                } catch (failure) {
                  report(failure);
                }
              }}
            />
          )}
          {!catalogue.capabilities.push && (
            <Row
              label="Why no background push"
              detail={catalogue.capabilities.push_reason}
            />
          )}
        </Group>
        <Group title="Paired devices">
          {catalogue.devices.map((device) => (
            <Row
              key={device.id}
              label={device.name}
              detail={
                device.id === catalogue.device ? "This device" : undefined
              }
            >
              {device.id !== catalogue.device && (
                <Button
                  variant="quiet"
                  size="compact"
                  onClick={() =>
                    command("revoke_device", null, { device_id: device.id })
                      .then(refresh)
                      .catch(report)
                  }
                >
                  Revoke
                </Button>
              )}
            </Row>
          ))}
          <Row label="Log out this device" destructive onClick={logout} />
        </Group>
      </>
    ),
    mcp: (
      <McpServers
        session={session}
        state={snapshots[selected]?.state}
        online={online}
      />
    ),
    advanced: null,
  };
  const current = section || "general";
  const title = SECTIONS.find(([id]) => id === current)![1];
  const drilled = phone && section !== null;
  return (
    <>
      <DialogHeader
        title={drilled ? title : "Settings"}
        leading={
          drilled && (
            <Button
              variant="quiet"
              class="settings-back"
              onClick={() => setSection(null)}
            >
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
                changed:
                  Number(theme !== DISPLAY.theme) +
                  Number(zoom !== DISPLAY.zoom) +
                  Number(timePrefs.clock !== defaultTimePrefs.clock) +
                  Number(timePrefs.style !== defaultTimePrefs.style),
                reset: () => {
                  setTheme(DISPLAY.theme);
                  setZoom(DISPLAY.zoom);
                  setTimePrefs(defaultTimePrefs);
                },
              }}
              filter={CONFIG[current as keyof typeof CONFIG]}
              fallback={<SettingRowsLoading />}
            />
          )}
          {pane[current]}
        </div>
      </div>
    </>
  );
}
